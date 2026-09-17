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

$BootAsm    = Join-Path $SrcDir "boards/pc-bios/boot.asm"
$LinkScript = Join-Path $SrcDir "boards/pc-bios/link.ld"
$IncludeDir = Join-Path $SrcDir "include"

$BootBin    = Join-Path $BuildDir "boot.bin"
$KernelBin  = Join-Path $BuildDir "kernel.bin"
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
    "hal/x86_64/apic.c",
    "hal/x86_64/smp.c",
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
        -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -target x86_64-elf `
        "-I$IncludeDir" -Wall -Wextra -c $src -o $obj
    if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: clang failed on $rel" -ForegroundColor Red; exit 1 }
    $ObjFiles += $obj
}

Write-Host "Linking (layout from src/boards/pc-bios/link.ld) ..."
ld.lld -m elf_x86_64 -T $LinkScript --oformat binary -o $KernelBin @ObjFiles
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: ld.lld failed." -ForegroundColor Red; exit 1 }

$BootSize   = (Get-Item $BootBin).Length
$KernelSize = (Get-Item $KernelBin).Length
Write-Host "boot.bin:   $BootSize bytes"
Write-Host "kernel.bin: $KernelSize bytes"

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
