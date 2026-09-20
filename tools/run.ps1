<#
    Vajra OS - build and boot in QEMU.

    Usage (from anywhere; works under Windows PowerShell and pwsh):
        tools/run.ps1                     build, then boot with the desktop window, 2 cores
        tools/run.ps1 -NoBuild            boot the existing build/disk.img as-is
        tools/run.ps1 -Headless -Seconds 20 -SerialLog build/boot.log
                                          no window, stop after 20s, capture serial output
        tools/run.ps1 -Smp 1              single core (the AP demo is skipped)
        tools/run.ps1 -Net                attach a virtio-net device (see note)

    -SerialLog also mirrors everything the kernel prints (the console
    writes every byte to COM1), which is what CI's logs are made from.

    Note on -Net: on this project's Windows QEMU (11.1.0) any run with
    -device virtio-net-pci stalls right after "IDT installed." (confirmed
    with an unmodified build; CI's Ubuntu QEMU 8.2.2 is fine). Two-instance
    networking is only verifiable on CI (.github/workflows/network-test.yml).
#>
param(
    [switch]$NoBuild,
    [switch]$Headless,
    [switch]$Net,
    [int]$Smp = 2,
    [int]$Seconds = 0,
    [string]$SerialLog = ""
)

$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Join-Path $PWD "tools" }
$RootDir   = Split-Path -Parent $ScriptDir
$DiskImg   = Join-Path $RootDir "build/disk.img"

if (-not $NoBuild) {
    $Shell = if (Get-Command pwsh -ErrorAction SilentlyContinue) { "pwsh" } else { "powershell" }
    & $Shell -File (Join-Path $ScriptDir "build-c.ps1")
    if ($LASTEXITCODE -ne 0) { Write-Host "Build failed -- not booting." -ForegroundColor Red; exit 1 }
}

if (-not (Test-Path $DiskImg)) {
    Write-Host "No $DiskImg -- run without -NoBuild first." -ForegroundColor Red
    exit 1
}

$QemuArgs = @(
    "-M", "pc", "-boot", "order=c",
    "-smp", "$Smp",
    "-drive", "file=$DiskImg,format=raw,if=ide",
    "-no-reboot"
)
if ($Headless)  { $QemuArgs += @("-display", "none") }
if ($SerialLog) { $QemuArgs += @("-serial", "file:$SerialLog") }
if ($Net)       { $QemuArgs += @("-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0") }

Write-Host "qemu-system-x86_64 $($QemuArgs -join ' ')" -ForegroundColor DarkGray

if ($Seconds -gt 0) {
    $p = Start-Process -FilePath qemu-system-x86_64 -ArgumentList $QemuArgs -PassThru -NoNewWindow
    Start-Sleep -Seconds $Seconds
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    if ($SerialLog) { Write-Host "Serial output: $SerialLog" }
} else {
    & qemu-system-x86_64 @QemuArgs
}
