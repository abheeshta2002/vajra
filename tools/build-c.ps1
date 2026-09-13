<#
    Vajra OS - C toolchain build script

    Structure this expects (see docs/folder-structure.md):
        src\boards\pc-bios\boot.asm   - BIOS-specific real-mode boot loader
        src\boards\pc-bios\link.ld    - linker script (load address tied to this board's loader)
        src\hal\x86_64\start.asm      - arch entry stub (stack setup -> call kernel_main)
        src\core\main.c               - portable kernel entry point and (eventually) all OS logic
        build\                        - ALL generated output lands here, nothing else

    Requires (see setup guide): NASM + QEMU on PATH, and MSYS2's
    mingw-w64-x86_64-gcc/-gdb/-make on PATH (installed from the
    "MSYS2 MinGW x64" shell specifically).
#>

$RootDir    = Split-Path -Parent $PSScriptRoot   # tools\ -> project root
$SrcDir     = Join-Path $RootDir "src"
$BuildDir   = Join-Path $RootDir "build"

$BootAsm    = Join-Path $SrcDir "boards\pc-bios\boot.asm"
$LinkScript = Join-Path $SrcDir "boards\pc-bios\link.ld"
$StartAsm   = Join-Path $SrcDir "hal\x86_64\start.asm"
$MainC      = Join-Path $SrcDir "core\main.c"
$IncludeDir = Join-Path $SrcDir "include"

$BootBin    = Join-Path $BuildDir "boot.bin"
$StartObj   = Join-Path $BuildDir "start.o"
$MainObj    = Join-Path $BuildDir "main.o"
$KernelBin  = Join-Path $BuildDir "kernel.bin"
$DiskImg    = Join-Path $BuildDir "disk.img"

function Assert-ToolOnPath($name) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        Write-Host "FATAL: '$name' not found on PATH. See the setup guide." -ForegroundColor Red
        exit 1
    }
}

Assert-ToolOnPath "nasm"
Assert-ToolOnPath "gcc"
Assert-ToolOnPath "ld"

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

Write-Host "Assembling boot loader (src\boards\pc-bios\boot.asm) ..."
nasm -f bin $BootAsm -o $BootBin
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: nasm failed on boot.asm" -ForegroundColor Red; exit 1 }

Write-Host "Assembling arch entry stub (src\hal\x86_64\start.asm) ..."
nasm -f elf64 $StartAsm -o $StartObj
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: nasm failed on start.asm" -ForegroundColor Red; exit 1 }

Write-Host "Compiling portable core (src\core\main.c) ..."
gcc -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie `
    -mno-red-zone -mcmodel=kernel -mgeneral-regs-only `
    "-I$IncludeDir" -Wall -Wextra -c $MainC -o $MainObj
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: gcc failed on main.c" -ForegroundColor Red; exit 1 }

Write-Host "Linking (layout from src\boards\pc-bios\link.ld) ..."
ld -m i386pep -T $LinkScript --oformat binary -o $KernelBin $StartObj $MainObj
if ($LASTEXITCODE -ne 0) {
    Write-Host "Retrying with elf_x86_64 emulation ..." -ForegroundColor Yellow
    ld -m elf_x86_64 -T $LinkScript --oformat binary -o $KernelBin $StartObj $MainObj
    if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: ld failed." -ForegroundColor Red; exit 1 }
}

$BootSize   = (Get-Item $BootBin).Length
$KernelSize = (Get-Item $KernelBin).Length
Write-Host "boot.bin:   $BootSize bytes"
Write-Host "kernel.bin: $KernelSize bytes"

if ($BootSize -ne 512) {
    Write-Host "FATAL: boot.bin must be exactly 512 bytes, got $BootSize" -ForegroundColor Red
    exit 1
}

Write-Host "Building build\disk.img ..."
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

Write-Host "build\disk.img: $((Get-Item $DiskImg).Length) bytes" -ForegroundColor Green
Write-Host ""
Write-Host "Build succeeded. To boot it:"
Write-Host "  qemu-system-x86_64 -drive file=`"$DiskImg`",format=raw,if=ide"
