[CmdletBinding()]
param(
    [string]$BuildDirectory = "build\release",
    [string]$OutputDirectory,
    [string]$Tag,
    [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Copy-QtNotices {
    param([string]$Module, [string]$Version, [string]$Destination)

    $treeIndex = @{}
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $root = [IO.Path]::GetFullPath($Destination)
    $tree = Get-GitHubJson "repos/qt/$Module/git/trees/v${Version}?recursive=1"
    if ($tree.truncated) { throw "The Qt attribution tree is truncated: $Module $Version" }
    foreach ($entry in $tree.tree) {
        if ($entry.type -eq "blob") { $treeIndex[$entry.path] = $entry.sha }
    }
    $attributionPaths = @($treeIndex.Keys | Where-Object { $_ -match "(^|/)qt_attribution\.json$" } | Sort-Object)
    $licensePaths = @($treeIndex.Keys | Where-Object { $_ -like "LICENSES/*" } | Sort-Object)
    if (-not $licensePaths) { throw "No upstream license files found for $Module $Version." }
    foreach ($path in $licensePaths) { [void]$paths.Add($path) }

    foreach ($path in $attributionPaths) {
        $bytes = Get-GitHubBlob $Module $treeIndex[$path]
        $destinationPath = Join-Path $root $path
        [void][IO.Directory]::CreateDirectory((Split-Path $destinationPath))
        [IO.File]::WriteAllBytes($destinationPath, $bytes)
        $records = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
        foreach ($record in @($records)) {
            foreach ($propertyName in @("LicenseFile", "LicenseFiles")) {
                $property = $record.PSObject.Properties[$propertyName]
                if ($null -eq $property) { continue }
                foreach ($licenseFile in @($property.Value)) {
                    $resolved = [IO.Path]::GetFullPath($licenseFile, (Split-Path $destinationPath))
                    if (-not $resolved.StartsWith("$root\", [StringComparison]::OrdinalIgnoreCase)) {
                        throw "Qt notice references a path outside its module: $path -> $licenseFile"
                    }
                    $relative = [IO.Path]::GetRelativePath($root, $resolved).Replace("\", "/")
                    if (-not $treeIndex.ContainsKey($relative)) {
                        throw "Qt notice references a missing license: $path -> $relative"
                    }
                    [void]$paths.Add($relative)
                }
            }
        }
    }
    foreach ($path in ($paths | Sort-Object)) {
        $bytes = Get-GitHubBlob $Module $treeIndex[$path]
        $destinationPath = Join-Path $root $path
        [void][IO.Directory]::CreateDirectory((Split-Path $destinationPath))
        [IO.File]::WriteAllBytes($destinationPath, $bytes)
    }
    [ordered]@{
        module = $Module
        source = "https://github.com/qt/$Module/tree/v$Version"
        tree = $tree.sha
        version = $Version
    }
}

function Get-GitHubBlob {
    param([string]$Module, [string]$Sha)

    $blob = Get-GitHubJson "repos/qt/$Module/git/blobs/$Sha"
    if ($blob.encoding -ne "base64") { throw "Unexpected GitHub blob encoding for $Module/$Sha." }
    return ,([Convert]::FromBase64String($blob.content))
}

function Get-GitHubJson {
    param([string]$Endpoint)

    $response = & gh api --cache 24h $Endpoint
    if ($LASTEXITCODE -ne 0) { throw "Unable to retrieve required attribution data: $Endpoint" }
    ($response -join "`n") | ConvertFrom-Json -Depth 100
}

$fileHashes = [ordered]@{}
$stableVersionPattern = "(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"
$sourceDirectory = Split-Path $PSScriptRoot
$repositoryDirectory = Split-Path $sourceDirectory
$cmakeContent = [IO.File]::ReadAllText((Join-Path $sourceDirectory "CMakeLists.txt"))
$cmakeVersions = [regex]::Matches(
    $cmakeContent,
    "(?m)^project\(dot[ \t]+LANGUAGES[ \t]+CXX[ \t]+VERSION[ \t]+([^ \t\r\n)]+)\)"
)
if ($cmakeVersions.Count -ne 1) { throw "Expected exactly one literal CMake project version." }
$version = $cmakeVersions[0].Groups[1].Value
$manifest = Get-Content -LiteralPath (Join-Path $sourceDirectory "vcpkg.json") -Raw | ConvertFrom-Json
if ($version -cnotmatch "\A$stableVersionPattern\z" -or $manifest.version -cne $version) {
    throw "CMake and vcpkg must contain the same stable MAJOR.MINOR.PATCH version."
}
if ($Tag -and ($Tag -cnotmatch "\Adot-v$stableVersionPattern\z" -or $Tag -cne "dot-v$version")) {
    throw "Expected desktop tag dot-v$version; received '$Tag'."
}
if ($ValidateOnly) {
    $version
    return
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    throw "Specify a new OutputDirectory for the release artifacts."
}

$build = [IO.Path]::GetFullPath($BuildDirectory, $sourceDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory, $sourceDirectory)
$cache = [IO.File]::ReadAllText((Join-Path $build "CMakeCache.txt"))
$sourceMatch = [regex]::Match($cache, "(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)\r?$")
if (-not $sourceMatch.Success -or
    [IO.Path]::GetFullPath($sourceMatch.Groups[1].Value.Trim()) -ne [IO.Path]::GetFullPath($sourceDirectory)) {
    throw "The build directory does not belong to this checkout: $build"
}
if ($cache -notmatch "(?m)^CMAKE_BUILD_TYPE:[^=\r\n]+=RelWithDebInfo\r?$" -or
    $cache -notmatch "(?m)^DOT_WERROR:BOOL=ON\r?$") {
    throw "Configure the release preset with -DDOT_WERROR=ON before packaging."
}
if (Test-Path -LiteralPath $output) {
    throw "OutputDirectory already exists; it will not be overwritten: $output"
}

$binary = Join-Path $build "RelWithDebInfo\dot.exe"
$binaryVersion = (& $binary --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $binaryVersion -cne "dot $version") {
    throw "The built executable version does not match dot $version."
}
& (Join-Path $sourceDirectory "tests\version_metadata_check.ps1") -ExePath $binary -ExpectedVersion $version

$timer = [Diagnostics.Stopwatch]::StartNew()
$stage = Join-Path $output "stage"
[void][IO.Directory]::CreateDirectory($stage)
& cmake --install $build --config RelWithDebInfo --prefix $stage | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Installing the portable runtime failed." }

$developmentFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
    $_.Extension -in @(".a", ".exp", ".ilk", ".lib", ".o", ".obj", ".pdb") -or
    ($_.Extension -eq ".exe" -and $_.FullName -ne (Join-Path $stage "bin\dot.exe"))
})
if ($developmentFiles) {
    throw "Development artifacts entered staging: $($developmentFiles.FullName -join ', ')"
}

$notices = Join-Path $stage "third_party"
$prefix = Join-Path $build "vcpkg_installed\x64-windows"
$packageLists = @(Get-ChildItem -LiteralPath (Join-Path $build "vcpkg_installed\vcpkg\info") -Filter "*.list" |
    Sort-Object Name)
$notedQtLibraries = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$qtSources = [Collections.Generic.List[object]]::new()
$runtimeNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$runtimeFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File -Filter "*.dll" | Sort-Object FullName)
foreach ($file in $runtimeFiles) { [void]$runtimeNames.Add($file.Name) }
Copy-Item -LiteralPath (Join-Path $repositoryDirectory "LICENSE") -Destination (Join-Path $stage "LICENSE")

foreach ($list in $packageLists) {
    if ($list.Name -notmatch "\A(?<package>[^_]+)_(?<version>.+)_x64-windows\.list\z") {
        throw "Unrecognized installed package record: $($list.Name)"
    }
    $packageName = $Matches.package
    $packageVersion = $Matches.version
    $installedFiles = [IO.File]::ReadAllLines($list.FullName)
    $copyright = Join-Path $prefix "share\$packageName\copyright"
    if (-not (Test-Path -LiteralPath $copyright -PathType Leaf) -or (Get-Item $copyright).Length -eq 0) {
        $codeFiles = @($installedFiles | Where-Object { $_ -match "\Ax64-windows/(bin|include|lib|Qt6)/.+[^/]\z" })
        if ($codeFiles.Count -eq 0) {
            Write-Host "No redistributed code in metadata-only package $packageName."
            continue
        }
        throw "Required dependency notice is missing: $packageName"
    }
    $packageDestination = Join-Path $notices $packageName
    [void][IO.Directory]::CreateDirectory($packageDestination)
    Copy-Item -LiteralPath $copyright -Destination (Join-Path $packageDestination "copyright")
    if ($packageName -cnotmatch "\Aqt[a-z0-9]+\z") { continue }
    $deployed = @($installedFiles | Where-Object {
        $_ -match "\.dll$" -and $runtimeNames.Contains([IO.Path]::GetFileName($_))
    })
    if (-not $deployed) { continue }
    if ($packageVersion -cnotmatch "\A$stableVersionPattern\z") {
        throw "Cannot resolve version-pinned Qt notices for $packageName $packageVersion."
    }
    Write-Host "Collecting upstream notices for $packageName $packageVersion."
    $qtSources.Add((Copy-QtNotices $packageName $packageVersion (Join-Path $packageDestination "upstream")))
    foreach ($path in $deployed) { [void]$notedQtLibraries.Add([IO.Path]::GetFileName($path)) }
}
foreach ($name in $runtimeNames) {
    if ($name -match "^Qt6" -and -not $notedQtLibraries.Contains($name)) {
        throw "No version-pinned attribution source was found for $name."
    }
}
if ($qtSources.Count -eq 0) { throw "No deployed Qt package was matched to its required notices." }

$registry = Get-Content -LiteralPath (Join-Path $sourceDirectory "vcpkg-configuration.json") -Raw | ConvertFrom-Json
$provenance = [ordered]@{
    qt = $qtSources.ToArray()
    registry = $registry."default-registry"
}
[IO.File]::WriteAllText(
    (Join-Path $notices "sources.json"),
    ($provenance | ConvertTo-Json -Depth 8) + "`n"
)
$copyrights = @($runtimeFiles | ForEach-Object {
    [ordered]@{
        copyright = $_.VersionInfo.LegalCopyright
        file = [IO.Path]::GetRelativePath($stage, $_.FullName)
        product = $_.VersionInfo.ProductName
        version = $_.VersionInfo.FileVersion
    }
})
[IO.File]::WriteAllText(
    (Join-Path $notices "binary_copyrights.json"),
    (ConvertTo-Json -InputObject $copyrights -Depth 4) + "`n"
)

$payloadFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName)
foreach ($file in $payloadFiles) {
    $relative = [IO.Path]::GetRelativePath($stage, $file.FullName)
    $fileHashes[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
}
$releaseManifest = [ordered]@{
    files = $fileHashes
    version = $version
}
[IO.File]::WriteAllText(
    (Join-Path $stage "release_manifest.json"),
    ($releaseManifest | ConvertTo-Json -Depth 5) + "`n"
)

$archive = Join-Path $output "dot-$version-win-x64.zip"
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $archive -CompressionLevel Optimal
$timer.Stop()
Write-Host "Packaged $($payloadFiles.Count) payload files in $($timer.Elapsed.TotalSeconds) seconds."
Write-Host "Archive bytes: $((Get-Item -LiteralPath $archive).Length)"
[pscustomobject][ordered]@{
    ArchivePath = $archive
    Sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    StagePath = $stage
    Version = $version
}
