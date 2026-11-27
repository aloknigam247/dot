using namespace System.Formats.Tar
using namespace System.IO.Compression
using namespace System.Security.Cryptography

[CmdletBinding()]
param([string]$RustcPath = $(if ($env:RUSTC) { $env:RUSTC } else { "rustc" }))

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-LockPackages([string]$Path) {
    foreach ($block in ([IO.File]::ReadAllText($Path) -csplit '(?m)^\[\[package\]\]\r?$')) {
        $fields = @{}
        foreach ($field in [regex]::Matches($block, '(?m)^(name|version|source|checksum) = "([^"\r\n]+)"\r?$')) {
            $fields[$field.Groups[1].Value] = $field.Groups[2].Value
        }
        if ($fields.ContainsKey("source")) { $fields }
    }
}

function Get-CrateNotices($Package, [string]$Checksum) {
    $declared = $null
    $foundDeclared = $false
    $name = $Package.name
    $version = $Package.version
    $prefix = "$name-$version/"
    $manifest = [IO.FileInfo]::new($Package.manifest_path)
    $crate = $manifest.Directory
    $index = $crate.Parent
    $archive = Join-Path $index.Parent.Parent.FullName "cache\$($index.Name)\$name-$version.crate"
    if ($manifest.Name -cne "Cargo.toml" -or $crate.Name -cne "$name-$version" -or
        $index.Parent.Name -ne "src" -or $index.Parent.Parent.Name -ne "registry" -or
        -not [IO.File]::Exists($archive)) {
        throw "Cargo archive not found for $name ${version}: $archive"
    }
    if ($Package.license_file) {
        $full = [IO.Path]::GetFullPath($Package.license_file, $crate.FullName)
        $declared = [IO.Path]::GetRelativePath($crate.FullName, $full).Replace("\", "/")
        if ([IO.Path]::IsPathRooted($declared) -or $declared -match '^\.\.(/|$)') {
            throw "Declared license file is outside the crate: $name $version $declared"
        }
    }

    $stream = [IO.File]::OpenRead($archive)
    try {
        if ([Convert]::ToHexString([SHA256]::HashData($stream)) -ne $Checksum) {
            throw "Cargo archive checksum mismatch for $name ${version}: $archive"
        }
        $stream.Position = 0
        $gzip = [GZipStream]::new($stream, [CompressionMode]::Decompress)
        try {
            $reader = [TarReader]::new($gzip)
            try {
                while ($null -ne ($entry = $reader.GetNextEntry())) {
                    if ($entry.EntryType -notin @([TarEntryType]::RegularFile, [TarEntryType]::V7RegularFile) -or
                        -not $entry.Name.StartsWith($prefix, [StringComparison]::Ordinal)) { continue }
                    $path = $entry.Name.Substring($prefix.Length)
                    if ($path -ceq $declared) {
                        $foundDeclared = $true
                    } elseif ($path -match '[/\\]' -or
                        $path -notmatch '^(COPYING|COPYRIGHT|LICEN[CS]E|NOTICE|UNLICENSE)') {
                        continue
                    }
                    $data = [IO.MemoryStream]::new()
                    try {
                        if ($null -ne $entry.DataStream) { $entry.DataStream.CopyTo($data) }
                        [pscustomobject]@{ Path = $path; Bytes = $data.ToArray() }
                    } finally { $data.Dispose() }
                }
            } finally { $reader.Dispose() }
        } finally { $gzip.Dispose() }
    } finally { $stream.Dispose() }
    if ($declared -and -not $foundDeclared) {
        throw "Declared license file missing from archive: $name $version $declared"
    }
}

function Get-StandardLibrary([string]$Compiler) {
    $info = & $Compiler -vV
    if ($LASTEXITCODE -ne 0) { throw "Unable to query the Rust compiler version: $Compiler" }
    $release = [regex]::Match(($info -join "`n"), '(?m)^release: (\S+)$')
    if (-not $release.Success) { throw "Missing Rust compiler release in $Compiler -vV." }
    $sysroot = & $Compiler --print sysroot
    if ($LASTEXITCODE -ne 0) { throw "Unable to locate the Rust sysroot: $Compiler" }
    $root = Join-Path $sysroot "share\doc\rust"
    $licenses = Join-Path $root "licenses"
    if (-not [IO.Directory]::Exists($licenses)) { throw "Missing Rust standard-library license directory: $licenses" }
    foreach ($required in @("COPYRIGHT-library.html", "licenses\Apache-2.0.txt", "licenses\MIT.txt")) {
        $path = Join-Path $root $required
        if (-not [IO.File]::Exists($path)) { throw "Missing Rust standard-library notice: $path" }
    }
    $files = @(Get-Item -LiteralPath (Join-Path $root "COPYRIGHT-library.html")
        Get-ChildItem -LiteralPath $licenses -Recurse -File)
    $notices = foreach ($file in ($files | Sort-Object FullName)) {
        [pscustomobject]@{
            Path = [IO.Path]::GetRelativePath($root, $file.FullName).Replace("\", "/")
            Bytes = [IO.File]::ReadAllBytes($file.FullName)
        }
    }
    [pscustomobject]@{ Name = "rust-std"; Version = $release.Groups[1].Value; Notices = $notices }
}

$target = "x86_64-pc-windows-msvc"
$crateDirectory = Join-Path (Split-Path $PSScriptRoot) "copilot_bridge"
Push-Location -LiteralPath $crateDirectory
try {
    $json = & cargo metadata --locked --format-version 1 --filter-platform $target
    if ($LASTEXITCODE -ne 0) { throw "cargo metadata failed for $target." }
    $metadata = $json | ConvertFrom-Json -Depth 100
    $tree = & cargo tree --locked --target $target --edges normal,build --prefix none --format "{p}" --color never
    if ($LASTEXITCODE -ne 0) { throw "cargo tree failed for $target." }
    $locked = @(Get-LockPackages (Join-Path $crateDirectory "Cargo.lock"))
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $records = @(foreach ($line in $tree) {
        if ($line -cnotmatch '^(\S+) v(\S+)(?:\s|$)') { throw "Unrecognized cargo tree entry: $line" }
        $name, $version = $Matches[1], $Matches[2]
        if (-not $seen.Add("$name $version")) { continue }
        $packages = @($metadata.packages | Where-Object { $_.name -ceq $name -and $_.version -ceq $version })
        if ($packages.Count -ne 1) { throw "Rust dependency is missing or ambiguous in cargo metadata: $name $version" }
        $package = $packages[0]
        if ($package.id -ceq $metadata.resolve.root) { continue }
        if ($package.source -cnotlike "registry+*") {
            throw "Unsupported Rust dependency source: $name $version $($package.source)"
        }
        $entries = @($locked | Where-Object {
            $_.name -ceq $name -and $_.version -ceq $version -and $_.source -ceq $package.source
        })
        if ($entries.Count -ne 1 -or $entries[0]["checksum"] -cnotmatch '^[a-f0-9]{64}$') {
            throw "Cargo.lock has no unique checksum for $name $version"
        }
        $notices = @(Get-CrateNotices $package $entries[0].checksum | Sort-Object Path)
        if (-not $notices.Count) { throw "No license notice found for Rust dependency $name $version" }
        [pscustomobject]@{ Name = $name; Version = $version; Notices = $notices }
    })
    @($records; Get-StandardLibrary $RustcPath) | Sort-Object Name, Version
} finally { Pop-Location }
