<#
    Vajra OS - Windows/PowerShell build script
    Mirrors build.sh: assembles boot.asm + kernel.asm, checks sizes,
    and produces a bootable disk.img.

    Expects boot.asm and kernel.asm in the same folder as this script
    (i.e. C:\Vajra). Requires NASM to be installed and on PATH.
#>

$ErrorActionPreference = "Stop"

# ---- Config (keep in sync with boot.asm's KERNEL_SECTORS) ----
$ReservedSectors = 50
$ReservedBytes   = $ReservedSectors * 512

# ---- Paths ----
$ProjectDir = "C:\Vajra"
$BootAsm    = Join-Path $ProjectDir "boot.asm"
$KernelAsm  = Join-Path $ProjectDir "kernel.asm"
$BootBin    = Join-Path $ProjectDir "boot.bin"
$KernelBin  = Join-Path $ProjectDir "kernel.bin"
$DiskImg    = Join-Path $ProjectDir "disk.img"

function Assert-ToolOnPath($name) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        Write-Error "'$name' was not found on PATH. Install it and try again (see notes at the bottom of this script)."
        exit 1
    }
}

Assert-ToolOnPath "nasm"

Write-Host "Assembling boot.asm ..."
nasm -f bin $BootAsm -o $BootBin
if ($LASTEXITCODE -ne 0) { Write-Error "nasm failed on boot.asm"; exit 1 }

Write-Host "Assembling kernel.asm ..."
nasm -f bin $KernelAsm -o $KernelBin
if ($LASTEXITCODE -ne 0) { Write-Error "nasm failed on kernel.asm"; exit 1 }

$BootSize   = (Get-Item $BootBin).Length
$KernelSize = (Get-Item $KernelBin).Length

Write-Host ("boot.bin:   {0} bytes" -f $BootSize)
Write-Host ("kernel.bin: {0} bytes  (reserved: {1} bytes / {2} sectors)" -f $KernelSize, $ReservedBytes, $ReservedSectors)

if ($BootSize -ne 512) {
    Write-Error "FATAL: boot.bin must be exactly 512 bytes, got $BootSize"
    exit 1
}

if ($KernelSize -gt $ReservedBytes) {
    Write-Error "FATAL: kernel.bin ($KernelSize bytes) exceeds reserved $ReservedBytes bytes ($ReservedSectors sectors)."
    Write-Error "        Increase KERNEL_SECTORS in boot.asm AND `$ReservedSectors in this script."
    exit 1
}

# ---- Build disk.img: boot.bin + kernel.bin, padded to 1.44MB ----
Write-Host "Building disk.img ..."
$bootBytes   = [System.IO.File]::ReadAllBytes($BootBin)
$kernelBytes = [System.IO.File]::ReadAllBytes($KernelBin)

$totalSize = $bootBytes.Length + $kernelBytes.Length
$padTarget = [Math]::Max(1474560, [Math]::Ceiling($totalSize / 512) * 512)
$padding   = New-Object byte[] ($padTarget - $totalSize)

$fs = [System.IO.File]::Open($DiskImg, [System.IO.FileMode]::Create)
try {
    $fs.Write($bootBytes, 0, $bootBytes.Length)
    $fs.Write($kernelBytes, 0, $kernelBytes.Length)
    $fs.Write($padding, 0, $padding.Length)
} finally {
    $fs.Close()
}

Write-Host ("disk.img: {0} bytes" -f (Get-Item $DiskImg).Length)
Write-Host ""
Write-Host "Build succeeded. To boot it (if QEMU is installed):"
Write-Host "  qemu-system-x86_64 -drive file=`"$DiskImg`",format=raw,if=ide -smp 2"

<#
    ---- Notes / one-time setup ----

    Install NASM:
      - via Chocolatey:  choco install nasm
      - or download from https://www.nasm.us/ and add its install
        folder (e.g. C:\Program Files\NASM) to your PATH.

    Install QEMU (optional, only needed to actually boot disk.img):
      - via Chocolatey:  choco install qemu
      - or download from https://www.qemu.org/download/#windows

    If 'nasm' or 'qemu-system-x86_64' aren't recognized after
    installing, open a NEW PowerShell window (PATH changes need a
    fresh shell) or add the install directory to PATH manually:
      $env:Path += ";C:\Program Files\NASM"
#>
