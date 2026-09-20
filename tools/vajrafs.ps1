<#
    Vajra OS - host-side file bridge for a disk image (build/disk.img).

    Puts files INTO Vajra's object store and gets them back OUT, without
    booting it. Run it with QEMU stopped -- it edits the image file directly.

        tools/vajrafs.ps1 -List
        tools/vajrafs.ps1 -Put notes.txt                 (stored as "notes.txt")
        tools/vajrafs.ps1 -Put C:\docs\a.txt -As todo    (stored as "todo")
        tools/vajrafs.ps1 -Get todo -Out C:\temp\todo.txt
        tools/vajrafs.ps1 -Remove todo

    Options: -Image path (default build/disk.img).

    Files put in this way are ordinary user-domain objects (the same as ones
    made with `edit`): UNTRUSTED, readable and editable by the shell and the
    utilities, and they cannot be run as programs unless they go through the
    quarantine pipeline. Limits (they are the object store's own): a name is
    1-39 characters (a path such as docs/a.txt is just a name), a file is at most 12288 bytes, at most 96 objects.

    The on-disk layout this reads/writes is core/storage.c's directory v3:
    magic 'VDR3' at LBA 400, 13 sectors, 64-byte entries (name = 40 bytes at +16); object data at
    LBA 420 + id*16 (24 sectors each).
#>
param(
    [string]$Image = "",
    [switch]$List,
    [string]$Put = "",
    [string]$As = "",
    [string]$Get = "",
    [string]$Out = "",
    [string]$Remove = ""
)

$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Join-Path $PWD "tools" }
if ($Image -eq "") { $Image = Join-Path (Split-Path -Parent $ScriptDir) "build/disk.img" }
if (-not (Test-Path $Image)) { Write-Host "No disk image at $Image" -ForegroundColor Red; exit 1 }

$SECTOR       = 512
$DIR_LBA      = 400
$DIR_SECTORS  = 13
$DATA_LBA     = 420
$SEC_PER_OBJ  = 24
$OBJ_MAX      = $SEC_PER_OBJ * $SECTOR
$MAX_OBJECTS  = 96
$ENTRY        = 64
$NAME_MAX     = 39
$MAGIC        = 0x33524456
$FIRST_USER_ID = 16   # ids below this are left to the kernel's own seeding (payload, programs, utilities)

$bytes = [System.IO.File]::ReadAllBytes($Image)
if ($bytes.Length -lt ($DATA_LBA + $MAX_OBJECTS * $SEC_PER_OBJ) * $SECTOR) {
    Write-Host "The image is too small to hold the object store." -ForegroundColor Red; exit 1
}
$dirOff = $DIR_LBA * $SECTOR

function Get-U32([int]$o) { return [System.BitConverter]::ToUInt32($bytes, $o) }
function Set-U32([int]$o, [uint32]$v) {
    $b = [System.BitConverter]::GetBytes($v)
    for ($i = 0; $i -lt 4; $i++) { $script:bytes[$o + $i] = $b[$i] }
}

# A never-booted image has no directory yet: start a blank v2 one.
if ((Get-U32 $dirOff) -ne $MAGIC) {
    for ($i = 0; $i -lt $DIR_SECTORS * $SECTOR; $i++) { $bytes[$dirOff + $i] = 0 }
    Set-U32 $dirOff $MAGIC
}
$count = [int]$bytes[$dirOff + 4]

function Entry-Off([int]$id) { return $dirOff + 8 + $id * $ENTRY }
function Entry-InUse([int]$id) { return $bytes[(Entry-Off $id)] -ne 0 }
function Entry-Name([int]$id) {
    $o = (Entry-Off $id) + 16
    $n = 0
    while ($n -lt $NAME_MAX -and $bytes[$o + $n] -ne 0) { $n++ }
    return [System.Text.Encoding]::ASCII.GetString($bytes, $o, $n)
}
function Find-Id([string]$name) {
    for ($i = 0; $i -lt $count; $i++) {
        if ((Entry-InUse $i) -and ((Entry-Name $i) -ceq $name)) { return $i }
    }
    return -1
}
function Save-Image { [System.IO.File]::WriteAllBytes($Image, $bytes) }
function Epoch2000 { return [uint32](([DateTime]::UtcNow - [DateTime]::new(2000, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)).TotalSeconds) }
function Fmt-Time([uint32]$t) {
    if ($t -eq 0) { return "----------------" }
    return ([DateTime]::new(2000, 1, 1, 0, 0, 0, [DateTimeKind]::Utc).AddSeconds($t)).ToString("yyyy-MM-dd HH:mm")
}
$trustNames = @("untrusted", "quarantined", "analyzed", "trusted", "REJECTED")

if ($List -or ($Put -eq "" -and $Get -eq "" -and $Remove -eq "")) {
    Write-Host ("{0,-3} {1,-24} {2,6}  {3,-11} {4,-6} {5}" -f "id", "name", "bytes", "trust", "domain", "modified")
    for ($i = 0; $i -lt $count; $i++) {
        if (-not (Entry-InUse $i)) { continue }
        $o = Entry-Off $i
        $trust = [int]$bytes[$o + 1]
        $tn = if ($trust -lt $trustNames.Length) { $trustNames[$trust] } else { "?" }
        $dom = if ($bytes[$o + 2]) { "user" } else { "system" }
        Write-Host ("{0,-3} {1,-24} {2,6}  {3,-11} {4,-6} {5}" -f $i, (Entry-Name $i), (Get-U32 ($o + 4)), $tn, $dom, (Fmt-Time (Get-U32 ($o + 12))))
    }
    exit 0
}

if ($Put -ne "") {
    if (-not (Test-Path $Put)) { Write-Host "No such file: $Put" -ForegroundColor Red; exit 1 }
    $data = [System.IO.File]::ReadAllBytes($Put)
    if ($data.Length -gt $OBJ_MAX) { Write-Host "$Put is $($data.Length) bytes; the limit is $OBJ_MAX." -ForegroundColor Red; exit 1 }
    $name = if ($As -ne "") { $As } else { [System.IO.Path]::GetFileName($Put) }
    if ($name.Length -lt 1 -or $name.Length -gt $NAME_MAX) { Write-Host "A name must be 1-$NAME_MAX characters (got '$name'; use -As)." -ForegroundColor Red; exit 1 }
    if ((Find-Id $name) -ge 0) { Write-Host "'$name' already exists in the image (use -Remove first)." -ForegroundColor Red; exit 1 }

    # first free id at or above FIRST_USER_ID; otherwise any free id
    $id = -1
    for ($i = $FIRST_USER_ID; $i -lt $MAX_OBJECTS; $i++) {
        if ($i -ge $count -or -not (Entry-InUse $i)) { $id = $i; break }
    }
    if ($id -lt 0) {
        for ($i = 0; $i -lt $count; $i++) { if (-not (Entry-InUse $i)) { $id = $i; break } }
    }
    if ($id -lt 0) { Write-Host "The object store is full (96 objects)." -ForegroundColor Red; exit 1 }

    $o = Entry-Off $id
    for ($i = 0; $i -lt $ENTRY; $i++) { $bytes[$o + $i] = 0 }
    $bytes[$o + 0] = 1   # in use
    $bytes[$o + 1] = 0   # UNTRUSTED
    $bytes[$o + 2] = 1   # user domain
    Set-U32 ($o + 4) ([uint32]$data.Length)
    $now = Epoch2000
    Set-U32 ($o + 8) $now
    Set-U32 ($o + 12) $now
    $nb = [System.Text.Encoding]::ASCII.GetBytes($name)
    for ($i = 0; $i -lt $nb.Length; $i++) { $bytes[$o + 16 + $i] = $nb[$i] }
    if ($id -ge $count) { $count = $id + 1; $bytes[$dirOff + 4] = [byte]$count }

    $dataOff = ($DATA_LBA + $id * $SEC_PER_OBJ) * $SECTOR
    for ($i = 0; $i -lt $data.Length; $i++) { $bytes[$dataOff + $i] = $data[$i] }
    Save-Image
    Write-Host "Put '$name' ($($data.Length) bytes) into $Image as object $id."
    exit 0
}

if ($Get -ne "") {
    $id = Find-Id $Get
    if ($id -lt 0) { Write-Host "No object named '$Get'." -ForegroundColor Red; exit 1 }
    $o = Entry-Off $id
    if ($bytes[$o + 1] -eq 4) { Write-Host "'$Get' was REJECTED by inspection; refusing to export it." -ForegroundColor Red; exit 1 }
    $size = [int](Get-U32 ($o + 4))
    $dataOff = ($DATA_LBA + $id * $SEC_PER_OBJ) * $SECTOR
    $dest = if ($Out -ne "") { $Out } else { $Get }
    $chunk = New-Object byte[] $size
    [System.Array]::Copy($bytes, $dataOff, $chunk, 0, $size)
    [System.IO.File]::WriteAllBytes($dest, $chunk)
    Write-Host "Wrote $size bytes of '$Get' to $dest."
    exit 0
}

if ($Remove -ne "") {
    $id = Find-Id $Remove
    if ($id -lt 0) { Write-Host "No object named '$Remove'." -ForegroundColor Red; exit 1 }
    $o = Entry-Off $id
    if (-not $bytes[$o + 2]) { Write-Host "'$Remove' is a system object; not removing it." -ForegroundColor Red; exit 1 }
    for ($i = 0; $i -lt $ENTRY; $i++) { $bytes[$o + $i] = 0 }
    Save-Image
    Write-Host "Removed '$Remove'."
    exit 0
}
