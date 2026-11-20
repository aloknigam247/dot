[CmdletBinding()]
param(
    [string]$CacheRoot = $(if ($env:DOT_GRAALVM_CACHE) { $env:DOT_GRAALVM_CACHE } else {
            Join-Path $env:LOCALAPPDATA "dot\graalvm"
        })
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

function Read-Pin([string]$Path) {
    $pin = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match "^\s*([a-z_0-9]+)\s*=\s*(.+?)\s*$") { $pin[$Matches[1]] = $Matches[2] }
    }
    foreach ($key in "native_image_version", "sha256", "url", "version") {
        if (-not $pin[$key]) { throw "Missing '$key' in $Path." }
    }
    $pin
}

function Assert-NoReparsePoint([string]$Path, [string]$Root) {
    $item = Get-Item -LiteralPath $Path -Force
    while ($item -and $item.FullName.TrimEnd("\") -ne $Root.TrimEnd("\")) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing linked path inside the GraalVM cache: $($item.FullName)"
        }
        $item = if ($item -is [IO.FileInfo]) { $item.Directory } else { $item.Parent }
    }
}

function Get-Sha256([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$pin = Read-Pin (Join-Path $PSScriptRoot "..\graalvm.properties")
$root = [IO.Path]::GetFullPath((Join-Path $CacheRoot $pin.version))
$archive = Join-Path $root ([IO.Path]::GetFileName(([uri]$pin.url).AbsolutePath))
$extracted = Join-Path $root "home"
$marker = Join-Path $root "extracted.sha256"

New-Item -ItemType Directory -Force -Path $root | Out-Null

if (-not (Test-Path -LiteralPath $archive) -or (Get-Sha256 $archive) -ne $pin.sha256) {
    $partial = "$archive.$([guid]::NewGuid()).part"
    try {
        Write-Host "Downloading $($pin.url)"
        Invoke-WebRequest -Uri $pin.url -OutFile $partial
        $actual = Get-Sha256 $partial
        if ($actual -ne $pin.sha256) { throw "GraalVM archive checksum mismatch: expected $($pin.sha256), got $actual." }
        Move-Item -LiteralPath $partial -Destination $archive -Force
    } finally {
        Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
    }
}

$recorded = if (Test-Path -LiteralPath $marker) { (Get-Content -LiteralPath $marker -Raw).Trim() } else { "" }
if ($recorded -ne $pin.sha256 -or -not (Test-Path -LiteralPath $extracted)) {
    $staging = "$extracted.$([guid]::NewGuid())"
    try {
        Write-Host "Extracting $archive"
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($archive, $staging)
        Remove-Item -LiteralPath $extracted -Recurse -Force -ErrorAction SilentlyContinue
        Move-Item -LiteralPath $staging -Destination $extracted
        [IO.File]::WriteAllText($marker, $pin.sha256)
    } finally {
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$homes = @(Get-ChildItem -LiteralPath $extracted -Directory -Force)
if ($homes.Count -ne 1) { throw "Expected one GraalVM directory in $extracted, found $($homes.Count)." }
$graalHome = $homes[0].FullName
$nativeImage = [IO.Path]::GetFullPath((Join-Path $graalHome "bin\native-image.cmd"))
if (-not (Test-Path -LiteralPath $nativeImage -PathType Leaf)) { throw "native-image is missing: $nativeImage" }
Assert-NoReparsePoint $nativeImage $root

$banner = (& $nativeImage --version 2>&1 | ForEach-Object { "$_".Trim() } | Where-Object { $_ }) -join " | "
if ($LASTEXITCODE -ne 0) { throw "native-image --version failed: $banner" }
if ($banner -ne $pin.native_image_version) {
    throw "native-image version mismatch: expected '$($pin.native_image_version)', got '$banner'."
}

[pscustomobject]@{
    Archive     = $archive
    Home        = $graalHome
    NativeImage = $nativeImage
    Version     = $banner
}
