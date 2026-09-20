<#
    Vajra OS - C toolchain build script

    Structure this expects (see docs/folder-structure.md):
        src/boards/pc-bios/boot.asm   - BIOS-specific real-mode boot loader
        src/boards/pc-bios/link.ld    - linker script (load address tied to this board's loader)
        src/hal/x86_64/*.asm / *.c    - x86-64 HAL (console, interrupts, memory map, context switch)
        src/core/*.c                  - portable kernel core (main, actor scheduler, memory manager)
        build/                        - ALL generated output lands here, nothing else

    Requires: NASM + QEMU on PATH, and clang + ld.lld (LLVM) on PATH.

    Note: MSYS2's mingw-w64 gcc/ld (mingw64/bin) cannot build this --
    its gcc rejects `-mcmodel=kernel` together with the PIE mode it
    defaults to, and its ld only emits PE, not a raw flat binary from
    ELF objects. clang -target x86_64-elf + ld.lld is what's verified
    working here.

    Cross-platform (Windows PowerShell and pwsh on Linux/macOS both
    run this unchanged -- forward slashes work as path separators on
    every platform .NET/PowerShell runs on, including Windows, so
    there's no OS-specific branch needed here). Verified via GitHub
    Actions (.github/workflows/) on Ubuntu, using apt-installed
    nasm/clang/lld/qemu-system-x86 instead of this repo's own Windows
    setup guide.

    -fno-jump-tables on every clang invocation, deliberately: every
    link here goes straight to `ld.lld --oformat binary` -- a RAW flat
    binary, not an ordinary ELF, so every address has to be fully
    resolved and baked in at link time; nothing survives for a loader
    to relocate afterward. clang's default codegen for a switch this
    wide (syscall_handler's own dispatch, 25 cases) emits a jump table
    -- a .rodata array of absolute case-target addresses, read and
    jumped through indirectly (`jmp [table + 8*index]`) instead of a
    compare-and-branch chain. On Ubuntu's specific ld.lld (Debian's
    apt build, a different version than this repo's own Windows LLVM
    install), one of those baked-in table entries came out wrong --
    confirmed by a real KERNEL PANIC on every CI boot, #UD at the
    exact address of an unrelated .bss array (paging.c's as_pml4),
    landed on via that indirect jump for syscall #1 specifically. This
    repo's own Windows toolchain resolves the same table correctly, so
    the bug never showed up locally -- only ever on CI, only after CI's
    OWN build step started working again (a separate, earlier fix).
    -fno-jump-tables forces the plain compare-and-branch form instead,
    sidestepping whatever this specific ld.lld build gets wrong about
    resolving that table for a flat binary, on every clang invocation
    here (not just the kernel's own switch) since any of them could
    plausibly hit the same class of bug for a sufficiently wide switch.
#>

$ScriptDir = $PSScriptRoot
if ([string]::IsNullOrEmpty($ScriptDir)) {
    # $PSScriptRoot is only set when this file is run directly (its own
    # process/dot-source). Running just a *selection* of it (e.g. VS
    # Code's "Run Selection") executes with no file context, so fall
    # back to the current directory -- this script is meant to be run
    # from the project root or from tools\ either way.
    $ScriptDir = if ((Split-Path -Leaf $PWD) -eq "tools") { $PWD } else { Join-Path $PWD "tools" }
    Write-Host "Note: `$PSScriptRoot was empty (likely ran as a selection, not the file) -- assuming '$ScriptDir'." -ForegroundColor Yellow
}
$RootDir    = Split-Path -Parent $ScriptDir      # tools\ -> project root
$SrcDir     = Join-Path $RootDir "src"
$BuildDir   = Join-Path $RootDir "build"

# This script itself already runs fine under either PowerShell edition
# (top comment above), but the VajraLang step below spawns a SEPARATE
# child process to run tools/vajrac.ps1 in, and that child process's
# executable name is NOT interchangeable the way running THIS script
# is: Windows PowerShell 5.1 is `powershell`, PowerShell Core (what
# Ubuntu/CI and this repo's own README both actually have) is `pwsh` --
# two different binaries, not two names for the same one. Hardcoding
# `powershell` here silently broke every CI build from the commit that
# added VajraLang onward (confirmed: every run since shows the "Build
# Vajra" step itself failing, not any test step after it) -- a real
# regression that went unnoticed because local verification that whole
# time was Windows-only QEMU boots, never a CI check. Picking whichever
# of the two actually exists on PATH fixes both environments without
# assuming which one is present.
$PwshExe = if (Get-Command pwsh -ErrorAction SilentlyContinue) { "pwsh" } else { "powershell" }

$BootAsm    = Join-Path $SrcDir "boards/pc-bios/boot.asm"
$LinkScript = Join-Path $SrcDir "boards/pc-bios/link.ld"
$IncludeDir = Join-Path $SrcDir "include"

$BootBin        = Join-Path $BuildDir "boot.bin"
$KernelBin      = Join-Path $BuildDir "kernel.bin"
$KernelDebugElf = Join-Path $BuildDir "kernel_debug.elf" # same objects, real ELF (symbols +
                                                          # section headers) instead of
                                                          # --oformat binary -- for nm/objdump,
                                                          # never booted or written to disk.img
$DiskImg    = Join-Path $BuildDir "disk.img"

# Every translation unit that makes up the kernel image, in link order.
# Add new HAL/core files here as they're added to the tree.
$AsmSources = @(
    "hal/x86_64/start.asm",
    "hal/x86_64/isr_stubs.asm",
    "hal/x86_64/context_switch.asm",
    "hal/x86_64/usermode.asm"
)
$CSources = @(
    "core/main.c",
    "core/actor.c",
    "core/memory.c",
    "core/storage.c",
    "core/net.c",
    "core/loader.c",
    "hal/x86_64/console.c",
    "hal/x86_64/e820.c",
    "hal/x86_64/interrupts.c",
    "hal/x86_64/gdt.c",
    "hal/x86_64/paging.c",
    "hal/x86_64/pic.c",
    "hal/x86_64/timer.c",
    "hal/x86_64/syscall.c",
    "hal/x86_64/syscall_invoke.c",
    "hal/x86_64/ata.c",
    "hal/x86_64/keyboard.c",
    "hal/x86_64/rtc.c",
    "hal/x86_64/mouse.c",
    "hal/x86_64/apic.c",
    "hal/x86_64/smp.c",
    "hal/x86_64/cpu.c",
    "hal/x86_64/acpi.c",
    "hal/x86_64/pci.c",
    "hal/x86_64/virtio_net.c"
)

# The AP trampoline (SMP bring-up, Milestone 12) is real-mode code
# that has to run from a fixed low physical address, not wherever it
# happens to land inside the main kernel image -- so it's assembled as
# its own standalone flat binary FIRST (like boot.asm), then wrapped as
# inert data (ap_trampoline_blob.asm's `incbin`) and linked into the
# kernel normally. See both files' own comments.
$ApTrampolineAsm = Join-Path $SrcDir "hal/x86_64/ap_trampoline.asm"
$ApTrampolineBin = Join-Path $BuildDir "ap_trampoline.bin"

function Assert-ToolOnPath($name) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        Write-Host "FATAL: '$name' not found on PATH. See the setup guide." -ForegroundColor Red
        exit 1
    }
}

Assert-ToolOnPath "nasm"
Assert-ToolOnPath "clang"
Assert-ToolOnPath "ld.lld"

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

Write-Host "Assembling boot loader (src/boards/pc-bios/boot.asm) ..."
nasm -f bin $BootAsm -o $BootBin
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: nasm failed on boot.asm" -ForegroundColor Red; exit 1 }

$ObjFiles = @()

Write-Host "Assembling AP trampoline (src/hal/x86_64/ap_trampoline.asm) ..."
nasm -f bin $ApTrampolineAsm -o $ApTrampolineBin
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: nasm failed on ap_trampoline.asm" -ForegroundColor Red; exit 1 }

$ApBlobAsm = Join-Path $SrcDir "hal/x86_64/ap_trampoline_blob.asm"
$ApBlobObj = Join-Path $BuildDir "ap_trampoline_blob.o"
Write-Host "Assembling hal/x86_64/ap_trampoline_blob.asm ..."
nasm -f elf64 -I "$BuildDir/" $ApBlobAsm -o $ApBlobObj
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: nasm failed on ap_trampoline_blob.asm" -ForegroundColor Red; exit 1 }
$ObjFiles += $ApBlobObj

# Roadmap Phase 16: build the "hello world" userland program as its
# OWN, wholly separate link (src/userland/program.ld, fixed at
# PROGRAM_VBASE) -- never part of kernel.bin's own C sources or link
# step above, the same way ap_trampoline.bin isn't. Then prepend the
# program_header (include/vajra/loader.h) tools/build-c.ps1 itself is
# responsible for, since the linker has no way to know its own final
# size to embed it -- and wrap the result as an incbin blob exactly
# like the AP trampoline, so kernel_main can seed it into a storage
# object at boot (core/main.c).
Write-Host "Building userland program: hello ..."
$UserlandDir  = Join-Path $SrcDir "userland"
$ProgramLd    = Join-Path $UserlandDir "program.ld"
$HelloRuntimeObj = Join-Path $BuildDir "userland_runtime.o"
$HelloObj     = Join-Path $BuildDir "userland_hello.o"
$HelloRawBin  = Join-Path $BuildDir "hello.raw.bin"
$HelloBin     = Join-Path $BuildDir "hello.bin"

clang -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie `
    -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -target x86_64-elf -fno-jump-tables `
    "-I$IncludeDir" -Wall -Wextra -c (Join-Path $UserlandDir "runtime.c") -o $HelloRuntimeObj
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: clang failed on userland/runtime.c" -ForegroundColor Red; exit 1 }

clang -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie `
    -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -target x86_64-elf -fno-jump-tables `
    "-I$IncludeDir" -Wall -Wextra -c (Join-Path $UserlandDir "hello.c") -o $HelloObj
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: clang failed on userland/hello.c" -ForegroundColor Red; exit 1 }

ld.lld -m elf_x86_64 -T $ProgramLd --oformat binary -o $HelloRawBin $HelloObj $HelloRuntimeObj
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: ld.lld failed linking userland/hello" -ForegroundColor Red; exit 1 }

$HelloRawBytes = [System.IO.File]::ReadAllBytes($HelloRawBin)
$HelloCodeSize = [uint32]$HelloRawBytes.Length

# program_header: magic(4) + entry_offset(4, always 0 -- _start lives
# in .text.start, program.ld places it first) + code_size(4), all
# little-endian, matching struct program_header's own field order.
$HelloHeader = New-Object byte[] 12
$MagicBytes      = [System.BitConverter]::GetBytes([uint32]0x524A4156)
$EntryOffsetBytes = [System.BitConverter]::GetBytes([uint32]0)
$CodeSizeBytes   = [System.BitConverter]::GetBytes($HelloCodeSize)
for ($i = 0; $i -lt 4; $i++) {
    $HelloHeader[$i]     = $MagicBytes[$i]
    $HelloHeader[$i + 4] = $EntryOffsetBytes[$i]
    $HelloHeader[$i + 8] = $CodeSizeBytes[$i]
}

$fs = [System.IO.File]::Open($HelloBin, [System.IO.FileMode]::Create)
try {
    $fs.Write($HelloHeader, 0, $HelloHeader.Length)
    $fs.Write($HelloRawBytes, 0, $HelloRawBytes.Length)
} finally {
    $fs.Close()
}
Write-Host "build/hello.bin: $((Get-Item $HelloBin).Length) bytes ($HelloCodeSize bytes of code+data)"

$HelloBlobAsm = Join-Path $SrcDir "hal/x86_64/hello_blob.asm"
$HelloBlobObj = Join-Path $BuildDir "hello_blob.o"
Write-Host "Assembling hal/x86_64/hello_blob.asm ..."
nasm -f elf64 -I "$BuildDir/" $HelloBlobAsm -o $HelloBlobObj
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: nasm failed on hello_blob.asm" -ForegroundColor Red; exit 1 }
$ObjFiles += $HelloBlobObj

# ------------------------------------------------------------------
# VajraLang: tools/vajrac.ps1 compiles src/userland/calc.vj into real
# C (build/calc_gen.c), which then goes through the EXACT SAME
# freestanding clang + ld.lld + program_header steps as hello.c just
# did above -- the whole point being that a loaded Vajra program does
# not care whether a human or vajrac wrote the C it started from.
# ------------------------------------------------------------------
Write-Host "Compiling VajraLang: src/userland/calc.vj ..."
$VajracScript = Join-Path $ScriptDir "vajrac.ps1"
$CalcVj       = Join-Path $UserlandDir "calc.vj"
$CalcGenC     = Join-Path $BuildDir "calc_gen.c"
& $PwshExe -File $VajracScript -InputPath $CalcVj -OutputPath $CalcGenC
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: vajrac failed on calc.vj" -ForegroundColor Red; exit 1 }

$CalcObj    = Join-Path $BuildDir "userland_calc.o"
$CalcRawBin = Join-Path $BuildDir "calc.raw.bin"
$CalcBin    = Join-Path $BuildDir "calc.bin"

clang -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie `
    -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -target x86_64-elf -fno-jump-tables `
    "-I$IncludeDir" -I $UserlandDir -Wall -Wextra -c $CalcGenC -o $CalcObj
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: clang failed on generated calc_gen.c" -ForegroundColor Red; exit 1 }

ld.lld -m elf_x86_64 -T $ProgramLd --oformat binary -o $CalcRawBin $CalcObj $HelloRuntimeObj
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: ld.lld failed linking userland/calc" -ForegroundColor Red; exit 1 }

$CalcRawBytes = [System.IO.File]::ReadAllBytes($CalcRawBin)
$CalcCodeSize = [uint32]$CalcRawBytes.Length
$CalcHeader = New-Object byte[] 12
$CalcMagicBytes = [System.BitConverter]::GetBytes([uint32]0x524A4156)
$CalcEntryOffsetBytes = [System.BitConverter]::GetBytes([uint32]0)
$CalcCodeSizeBytes = [System.BitConverter]::GetBytes($CalcCodeSize)
for ($i = 0; $i -lt 4; $i++) {
    $CalcHeader[$i]     = $CalcMagicBytes[$i]
    $CalcHeader[$i + 4] = $CalcEntryOffsetBytes[$i]
    $CalcHeader[$i + 8] = $CalcCodeSizeBytes[$i]
}
$fs = [System.IO.File]::Open($CalcBin, [System.IO.FileMode]::Create)
try {
    $fs.Write($CalcHeader, 0, $CalcHeader.Length)
    $fs.Write($CalcRawBytes, 0, $CalcRawBytes.Length)
} finally {
    $fs.Close()
}
Write-Host "build/calc.bin: $((Get-Item $CalcBin).Length) bytes ($CalcCodeSize bytes of code+data)"

$CalcBlobAsm = Join-Path $SrcDir "hal/x86_64/calc_blob.asm"
$CalcBlobObj = Join-Path $BuildDir "calc_blob.o"
Write-Host "Assembling hal/x86_64/calc_blob.asm ..."
nasm -f elf64 -I "$BuildDir/" $CalcBlobAsm -o $CalcBlobObj
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: nasm failed on calc_blob.asm" -ForegroundColor Red; exit 1 }
$ObjFiles += $CalcBlobObj

foreach ($rel in $AsmSources) {
    $src = Join-Path $SrcDir $rel
    $obj = Join-Path $BuildDir ((Split-Path -Leaf $rel) -replace '\.asm$', '.o')
    Write-Host "Assembling $rel ..."
    nasm -f elf64 $src -o $obj
    if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: nasm failed on $rel" -ForegroundColor Red; exit 1 }
    $ObjFiles += $obj
}

foreach ($rel in $CSources) {
    $src = Join-Path $SrcDir $rel
    $obj = Join-Path $BuildDir ((Split-Path -Leaf $rel) -replace '\.c$', '.o')
    Write-Host "Compiling $rel ..."
    clang -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie `
        -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -target x86_64-elf -fno-jump-tables `
        "-I$IncludeDir" -Wall -Wextra -c $src -o $obj
    if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: clang failed on $rel" -ForegroundColor Red; exit 1 }
    $ObjFiles += $obj
}

Write-Host "Linking (layout from src/boards/pc-bios/link.ld) ..."
ld.lld -m elf_x86_64 -T $LinkScript --oformat binary -o $KernelBin @ObjFiles
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: ld.lld failed." -ForegroundColor Red; exit 1 }

# Same objects, same link script, but a real ELF (keeps the symbol
# table and section headers --oformat binary above deliberately
# strips) -- not part of the bootable image at all, purely so `nm`/
# `objdump` can answer "what's actually at this address" when
# something goes wrong (e.g. a QEMU debug log's own RIP/CR2), on
# whichever machine actually built the kernel -- symbol addresses are
# a function of THIS toolchain's own codegen/layout choices and can
# genuinely differ from another machine's build of the identical
# source (confirmed the hard way: a real KERNEL PANIC on CI's Ubuntu
# build needed exactly this file, which until now only existed as an
# ad hoc one-off, never actually produced by this script).
ld.lld -m elf_x86_64 -T $LinkScript -o $KernelDebugElf @ObjFiles
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: ld.lld failed building the debug ELF." -ForegroundColor Red; exit 1 }

$BootSize   = (Get-Item $BootBin).Length
$KernelSize = (Get-Item $KernelBin).Length
Write-Host "boot.bin:   $BootSize bytes"
Write-Host "kernel.bin: $KernelSize bytes"

# The boot loader reads exactly KERNEL_SECTORS sectors (src/boards/pc-bios/
# boot.asm, fix #6) and silently stops there -- a kernel.bin larger than
# that boots "fine" with its tail (late .rodata string literals, all of
# .data) read back as zeros, which surfaced as data-dependent, logic-
# looking bugs (blank object names, empty object contents, a failing
# loader) on CI's Ubuntu toolchain, whose clang emits ~4KB more code than
# this repo's own Windows LLVM for the same source. Refuse it here, loudly,
# instead of discovering it from symptoms; warn early when close.
$BootAsmText = Get-Content -Raw $BootAsm
if ($BootAsmText -notmatch '(?m)^KERNEL_SECTORS\s+equ\s+(\d+)') {
    Write-Host "FATAL: could not read KERNEL_SECTORS from boot.asm" -ForegroundColor Red; exit 1
}
$KernelCapBytes = [int]$Matches[1] * 512
if ($KernelSize -gt $KernelCapBytes) {
    Write-Host "FATAL: kernel.bin ($KernelSize bytes) exceeds the boot loader's KERNEL_SECTORS cap ($KernelCapBytes bytes) -- the tail would be silently truncated at boot. Raise KERNEL_SECTORS (and core/storage.c's DIRECTORY_LBA/OBJECT_DATA_BASE_LBA past it)." -ForegroundColor Red
    exit 1
}
if ($KernelSize -gt ($KernelCapBytes * 0.8)) {
    Write-Host "WARNING: kernel.bin is over 80% of the boot loader's $KernelCapBytes-byte cap ($KernelSize bytes)." -ForegroundColor Yellow
}

# .bss (page tables + kernel stacks, ~20KB per MAX_ACTORS slot) is zeroed
# at boot rather than loaded, so kernel.bin's size says nothing about it --
# but it must still end below 0x9F000 (the BIOS's EBDA / the VGA window at
# 0xA0000). Raising MAX_ACTORS to 24 once pushed it to 0xB5000: no build
# error, just a #PF inside the APIC setup at boot. Catch that here.
$LlvmNm = Get-Command llvm-nm -ErrorAction SilentlyContinue
if ($LlvmNm -and (Test-Path $KernelDebugElf)) {
    $BssEndLine = & $LlvmNm.Source $KernelDebugElf | Where-Object { $_ -match ' __bss_end$' }
    if ($BssEndLine -match '^([0-9a-fA-F]+)') {
        $BssEnd = [Convert]::ToInt64($Matches[1], 16)
        Write-Host ("kernel .bss ends at 0x{0:X}" -f $BssEnd)
        if ($BssEnd -gt 0x9F000) {
            Write-Host ("FATAL: kernel .bss ends at 0x{0:X}, past the 0x9F000 limit (EBDA/VGA window). Lower MAX_ACTORS (src/include/vajra/actor.h)." -f $BssEnd) -ForegroundColor Red
            exit 1
        }
    }
}

if ($BootSize -ne 512) {
    Write-Host "FATAL: boot.bin must be exactly 512 bytes, got $BootSize" -ForegroundColor Red
    exit 1
}

Write-Host "Building build/disk.img ..."
$bootBytes   = [System.IO.File]::ReadAllBytes($BootBin)
$kernelBytes = [System.IO.File]::ReadAllBytes($KernelBin)
$totalSize   = $bootBytes.Length + $kernelBytes.Length
$padTarget   = [Math]::Max(1474560, [Math]::Ceiling($totalSize / 512) * 512)
$padding     = New-Object byte[] ($padTarget - $totalSize)

$fs = [System.IO.File]::Open($DiskImg, [System.IO.FileMode]::Create)
try {
    $fs.Write($bootBytes, 0, $bootBytes.Length)
    $fs.Write($kernelBytes, 0, $kernelBytes.Length)
    $fs.Write($padding, 0, $padding.Length)
} finally {
    $fs.Close()
}

Write-Host "build/disk.img: $((Get-Item $DiskImg).Length) bytes" -ForegroundColor Green
Write-Host ""
Write-Host "Build succeeded. To boot it:"
Write-Host "  qemu-system-x86_64 -drive file=`"$DiskImg`",format=raw,if=ide"
