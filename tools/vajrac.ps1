<#
    vajrac -- the VajraLang compiler.

    A real front end (lexer, recursive-descent parser, a small AST) for
    a tiny calculator language, targeting C as its backend: codegen
    emits a .c file written against src/userland/runtime.h's own
    syscall wrappers, which then goes through the EXACT SAME clang +
    ld.lld + program_header pipeline tools/build-c.ps1 already uses for
    src/userland/hello.c -- the real, already-verified path from a flat
    binary to a loaded, running Vajra actor (include/vajra/loader.h).
    Targeting C instead of hand-emitted x86-64 machine code is a
    completely ordinary compiler architecture (early C++, Nim, and
    plenty of production languages do exactly this).

    Written in PowerShell, not C: this machine's clang has no host C
    standard library configured (targets x86_64-pc-windows-msvc with no
    Windows SDK / MSVC installed, and no mingw gcc either) -- only the
    freestanding -target x86_64-elf builds tools/build-c.ps1 already
    uses work here. PowerShell is guaranteed present (it's what runs
    this whole build), so the compiler's IMPLEMENTATION language moved,
    not its architecture: still a real lexer -> parser -> AST -> codegen
    pipeline, still emits real C for the real freestanding toolchain to
    turn into a real x86-64 program.

    Language, v0:
      program    := statement*
      statement  := "let" IDENT "=" expr ";"
                  | "print" expr ";"
      expr       := term (("+" | "-") term)*
      term       := unary (("*" | "/") unary)*
      unary      := "-" unary | primary
      primary    := NUMBER | IDENT | "(" expr ")"

    Integers only. Variables are plain C locals in the generated
    _start(): no real symbol table needed at this scale, just a check
    that a name was declared with `let` before it's read, so a typo is
    a compile error here, not a silent zero at runtime.
#>

param(
    [Parameter(Mandatory=$true)][string]$InputPath,
    [Parameter(Mandatory=$true)][string]$OutputPath
)

$Source = Get-Content -Raw -Path $InputPath
if ($null -eq $Source) { $Source = "" }

# ---- Lexer ----

$TokKinds = @{
    Num = "Num"; Ident = "Ident"; Let = "Let"; Print = "Print"
    Plus = "Plus"; Minus = "Minus"; Star = "Star"; Slash = "Slash"
    LParen = "LParen"; RParen = "RParen"; Equals = "Equals"; Semi = "Semi"; Eof = "Eof"
}

$script:Pos = 0
$script:Line = 1
$Len = $Source.Length

function New-Token($Kind, $Value) {
    return [PSCustomObject]@{ Kind = $Kind; Value = $Value; Line = $script:Line }
}

function Lex-Next {
    while ($script:Pos -lt $Len) {
        $c = $Source[$script:Pos]
        if ($c -eq "`n") { $script:Line++; $script:Pos++; continue }
        if ($c -eq ' ' -or $c -eq "`t" -or $c -eq "`r") { $script:Pos++; continue }
        if ($c -eq '#') { while ($script:Pos -lt $Len -and $Source[$script:Pos] -ne "`n") { $script:Pos++ }; continue }
        break
    }
    if ($script:Pos -ge $Len) { return New-Token $TokKinds.Eof $null }

    $c = $Source[$script:Pos]

    if ([char]::IsDigit($c)) {
        $start = $script:Pos
        while ($script:Pos -lt $Len -and [char]::IsDigit($Source[$script:Pos])) { $script:Pos++ }
        $text = $Source.Substring($start, $script:Pos - $start)
        return New-Token $TokKinds.Num ([long]$text)
    }

    if ([char]::IsLetter($c) -or $c -eq '_') {
        $start = $script:Pos
        while ($script:Pos -lt $Len -and ([char]::IsLetterOrDigit($Source[$script:Pos]) -or $Source[$script:Pos] -eq '_')) { $script:Pos++ }
        $text = $Source.Substring($start, $script:Pos - $start)
        if ($text -eq "let") { return New-Token $TokKinds.Let $null }
        if ($text -eq "print") { return New-Token $TokKinds.Print $null }
        return New-Token $TokKinds.Ident $text
    }

    $script:Pos++
    switch ($c) {
        '+' { return New-Token $TokKinds.Plus $null }
        '-' { return New-Token $TokKinds.Minus $null }
        '*' { return New-Token $TokKinds.Star $null }
        '/' { return New-Token $TokKinds.Slash $null }
        '(' { return New-Token $TokKinds.LParen $null }
        ')' { return New-Token $TokKinds.RParen $null }
        '=' { return New-Token $TokKinds.Equals $null }
        ';' { return New-Token $TokKinds.Semi $null }
        default {
            Write-Host "vajrac: line $($script:Line): unexpected character '$c'" -ForegroundColor Red
            exit 1
        }
    }
}

# ---- Parser (recursive descent, one token of lookahead) ----

$script:Cur = $null
$script:Declared = New-Object System.Collections.Generic.HashSet[string]

function Advance { $script:Cur = Lex-Next }

function Expect($Kind, $What) {
    if ($script:Cur.Kind -ne $Kind) {
        Write-Host "vajrac: line $($script:Cur.Line): expected $What" -ForegroundColor Red
        exit 1
    }
}

function New-Node($Kind) {
    return [PSCustomObject]@{ Kind = $Kind; Num = 0; Var = $null; Op = $null; Lhs = $null; Rhs = $null }
}

function Parse-Primary {
    if ($script:Cur.Kind -eq $TokKinds.Num) {
        $n = New-Node "Num"; $n.Num = $script:Cur.Value
        Advance
        return $n
    }
    if ($script:Cur.Kind -eq $TokKinds.Ident) {
        if (-not $script:Declared.Contains($script:Cur.Value)) {
            Write-Host "vajrac: line $($script:Cur.Line): '$($script:Cur.Value)' used before it was declared with 'let'" -ForegroundColor Red
            exit 1
        }
        $n = New-Node "Var"; $n.Var = $script:Cur.Value
        Advance
        return $n
    }
    if ($script:Cur.Kind -eq $TokKinds.LParen) {
        Advance
        $n = Parse-Expr
        Expect $TokKinds.RParen "')'"
        Advance
        return $n
    }
    Write-Host "vajrac: line $($script:Cur.Line): expected a number, a variable, or '('" -ForegroundColor Red
    exit 1
}

function Parse-Unary {
    if ($script:Cur.Kind -eq $TokKinds.Minus) {
        Advance
        $n = New-Node "Neg"; $n.Lhs = Parse-Unary
        return $n
    }
    return Parse-Primary
}

function Parse-Term {
    $n = Parse-Unary
    while ($script:Cur.Kind -eq $TokKinds.Star -or $script:Cur.Kind -eq $TokKinds.Slash) {
        $op = if ($script:Cur.Kind -eq $TokKinds.Star) { '*' } else { '/' }
        Advance
        $rhs = Parse-Unary
        $bin = New-Node "Bin"; $bin.Op = $op; $bin.Lhs = $n; $bin.Rhs = $rhs
        $n = $bin
    }
    return $n
}

function Parse-Expr {
    $n = Parse-Term
    while ($script:Cur.Kind -eq $TokKinds.Plus -or $script:Cur.Kind -eq $TokKinds.Minus) {
        $op = if ($script:Cur.Kind -eq $TokKinds.Plus) { '+' } else { '-' }
        Advance
        $rhs = Parse-Term
        $bin = New-Node "Bin"; $bin.Op = $op; $bin.Lhs = $n; $bin.Rhs = $rhs
        $n = $bin
    }
    return $n
}

function Parse-Program {
    $stmts = New-Object System.Collections.Generic.List[object]
    Advance
    while ($script:Cur.Kind -ne $TokKinds.Eof) {
        if ($script:Cur.Kind -eq $TokKinds.Let) {
            Advance
            Expect $TokKinds.Ident "a variable name"
            $varName = $script:Cur.Value
            Advance
            Expect $TokKinds.Equals "'='"
            Advance
            $expr = Parse-Expr
            Expect $TokKinds.Semi "';'"
            Advance
            $script:Declared.Add($varName) | Out-Null
            $stmts.Add([PSCustomObject]@{ Kind = "Let"; Var = $varName; Expr = $expr })
        } elseif ($script:Cur.Kind -eq $TokKinds.Print) {
            Advance
            $expr = Parse-Expr
            Expect $TokKinds.Semi "';'"
            Advance
            $stmts.Add([PSCustomObject]@{ Kind = "Print"; Expr = $expr })
        } else {
            Write-Host "vajrac: line $($script:Cur.Line): expected 'let' or 'print'" -ForegroundColor Red
            exit 1
        }
    }
    return $stmts
}

# ---- Codegen: AST -> a C expression string, then a full _start() ----

function Gen-Expr($Node) {
    switch ($Node.Kind) {
        "Num" { return "$($Node.Num)LL" }
        "Var" { return $Node.Var }
        "Neg" { return "(-$(Gen-Expr $Node.Lhs))" }
        "Bin" { return "($(Gen-Expr $Node.Lhs) $($Node.Op) $(Gen-Expr $Node.Rhs))" }
    }
}

function Codegen($Stmts) {
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("/* Generated by tools/vajrac.ps1 -- do not edit by hand. */")
    [void]$sb.AppendLine('#include "runtime.h"')
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine('__attribute__((section(".text.start")))')
    [void]$sb.AppendLine("void _start(void) {")
    foreach ($s in $Stmts) {
        if ($s.Kind -eq "Let") {
            [void]$sb.AppendLine("    long long $($s.Var) = $(Gen-Expr $s.Expr);")
        } else {
            [void]$sb.AppendLine("    user_write_int($(Gen-Expr $s.Expr));")
            [void]$sb.AppendLine('    user_write("\n");')
        }
    }
    [void]$sb.AppendLine("    user_exit();")
    [void]$sb.AppendLine("}")
    return $sb.ToString()
}

$program = Parse-Program
$code = Codegen $program
Set-Content -Path $OutputPath -Value $code -NoNewline -Encoding ascii
Write-Host "vajrac: compiled '$InputPath' -> '$OutputPath'"
