[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ArchivePath,
    [Parameter(Mandatory)][string]$ExpectedVersion,
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 60
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$bootLog = $null
$expectedDlls = @{}
$loadedQt = $false
$process = $null
$ready = $false
$sourceDirectory = Split-Path $PSScriptRoot
$archive = (Resolve-Path -LiteralPath $ArchivePath).Path
if ([IO.Path]::GetFileName($archive) -cne "dot-$ExpectedVersion-win-x64.zip") {
    throw "The archive filename does not match the expected version."
}
$nvim = (Get-Command nvim -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$id = [guid]::NewGuid().ToString()
$work = Join-Path $sourceDirectory "tmp\verify_release_$id"
$extracted = Join-Path $work "extracted"
[void][IO.Directory]::CreateDirectory($work)

try {
    [IO.Compression.ZipFile]::ExtractToDirectory($archive, $extracted)
    $manifestPath = Join-Path $extracted "release_manifest.json"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.version -cne $ExpectedVersion) { throw "The archive manifest version does not match." }
    $expectedFiles = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $manifest.files.PSObject.Properties) {
        $path = [IO.Path]::GetFullPath($entry.Name, $extracted)
        if (-not $path.StartsWith("$extracted\", [StringComparison]::OrdinalIgnoreCase)) {
            throw "The manifest contains an unsafe path: $($entry.Name)"
        }
        if ($entry.Value -cnotmatch "\A[0-9A-F]{64}\z") { throw "Invalid manifest hash: $($entry.Name)" }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Required archive file is missing: $($entry.Name)"
        }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -cne $entry.Value) {
            throw "Archive file hash mismatch: $($entry.Name)"
        }
        [void]$expectedFiles.Add($path)
        if ([IO.Path]::GetExtension($path) -eq ".dll") {
            $name = [IO.Path]::GetFileName($path)
            if (-not $expectedDlls.ContainsKey($name)) {
                $expectedDlls[$name] = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            }
            [void]$expectedDlls[$name].Add($path)
        }
    }
    if ($expectedFiles.Count -eq 0) { throw "The archive manifest is empty." }
    [void]$expectedFiles.Add($manifestPath)
    foreach ($file in (Get-ChildItem -LiteralPath $extracted -Recurse -File)) {
        if (-not $expectedFiles.Contains($file.FullName)) { throw "Unlisted archive file: $($file.FullName)" }
        if ($file.Extension -in @(".a", ".exp", ".ilk", ".lib", ".o", ".obj", ".pdb") -or
            ($file.Extension -eq ".exe" -and $file.FullName -ne (Join-Path $extracted "bin\dot.exe"))) {
            throw "Development artifact in archive: $($file.FullName)"
        }
    }
    foreach ($relative in @(
        "bin\dot.exe",
        "bin\msvcp140.dll",
        "bin\qt.conf",
        "bin\Qt6Core.dll",
        "bin\vcruntime140.dll",
        "bin\vcruntime140_1.dll",
        "LICENSE",
        "Qt6\plugins\platforms\qwindows.dll",
        "Qt6\qml\QtQml\qmldir",
        "Qt6\qml\QtQuick\qmldir",
        "third_party\binary_copyrights.json",
        "third_party\sources.json"
    )) {
        if (-not $expectedFiles.Contains((Join-Path $extracted $relative))) {
            throw "Required runtime content is absent from the manifest: $relative"
        }
    }

    $binary = Join-Path $extracted "bin\dot.exe"
    & (Join-Path $sourceDirectory "tests\version_metadata_check.ps1") -ExePath $binary -ExpectedVersion $ExpectedVersion
    $binaryVersion = (& $binary --version | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $binaryVersion -cne "dot $ExpectedVersion") {
        throw "The extracted executable version does not match."
    }

    $attached = Join-Path $work "attached_$id.txt"
    $bootLog = Join-Path $work "boot_$id.log"
    $fixture = Join-Path $work "init_$id.vim"
    $sample = Join-Path $work "sample_$id.txt"
    [IO.File]::WriteAllText(
        $fixture,
        "set nomore`nautocmd UIEnter * call writefile(['attached'], `$DOT_RELEASE_ATTACH_FILE)`n"
    )
    [IO.File]::WriteAllText($sample, "dot portable release verification`nBundled Qt and clean Neovim.`n")
    $start = [Diagnostics.ProcessStartInfo]::new($binary)
    $start.UseShellExecute = $false
    $start.WorkingDirectory = $extracted
    foreach ($key in @($start.Environment.Keys)) {
        if ($key -match "^(ACTIONS_|DOT_|GH_|GITHUB_|NVIM|QML|QSG_|QT_)") {
            [void]$start.Environment.Remove($key)
        }
    }
    $start.Environment["DOT_BOOT_PROFILE"] = "1"
    $start.Environment["DOT_BOOT_PROFILE_FILE"] = $bootLog
    $start.Environment["DOT_RELEASE_ATTACH_FILE"] = $attached
    $start.Environment["NVIM_LOG_FILE"] = Join-Path $work "nvim_$id.log"
    $start.Environment["PATH"] = @(
        (Join-Path $extracted "bin"),
        (Split-Path $nvim),
        (Join-Path $env:SystemRoot "System32"),
        $env:SystemRoot
    ) -join [IO.Path]::PathSeparator
    $start.Environment["QT_QPA_PLATFORM"] = "windows"
    foreach ($kind in @("CACHE", "CONFIG", "DATA", "STATE")) {
        $start.Environment["XDG_${kind}_HOME"] = Join-Path $work $kind.ToLowerInvariant()
    }
    foreach ($argument in @("--clean", "-u", $fixture, "-i", "NONE", "-n", $sample)) {
        $start.ArgumentList.Add($argument)
    }

    $timer = [Diagnostics.Stopwatch]::StartNew()
    $process = [Diagnostics.Process]::Start($start)
    while ($timer.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        if ($process.HasExited) { throw "The extracted application exited early: $($process.ExitCode)" }
        if ((Test-Path -LiteralPath $bootLog) -and (Test-Path -LiteralPath $attached)) {
            $boot = Get-Content -LiteralPath $bootLog -Raw
            if ($boot.Contains("window shown at real size") -and $boot.Contains("first frame swapped")) {
                $ready = $true
                break
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $ready) { throw "The extracted application did not show and render within $TimeoutSeconds seconds." }
    $process.Refresh()
    if ($process.MainWindowHandle -eq 0) { throw "No native application window was created." }
    foreach ($module in $process.Modules) {
        if ($expectedDlls.ContainsKey($module.ModuleName) -and
            -not $expectedDlls[$module.ModuleName].Contains([IO.Path]::GetFullPath($module.FileName))) {
            throw "A bundled dependency was loaded from outside the archive: $($module.FileName)"
        }
        if ($module.ModuleName -ieq "Qt6Core.dll") { $loadedQt = $true }
    }
    if (-not $loadedQt) { throw "Unable to observe the extracted Qt runtime in the live process." }
    if (-not $process.CloseMainWindow() -or -not $process.WaitForExit(10000)) {
        throw "The extracted application did not close normally."
    }
    if ($process.ExitCode -ne 0) { throw "The extracted application exited with code $($process.ExitCode)." }
}
finally {
    if ($null -ne $process) {
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force
            $process.WaitForExit()
        }
        $process.Dispose()
    }
    if ($bootLog -and (Test-Path -LiteralPath $bootLog)) {
        Write-Host (Get-Content -LiteralPath $bootLog -Raw)
    }
    Remove-Item -LiteralPath $work -Recurse -Force
}
Write-Host "PASS: extracted dot $ExpectedVersion attached, showed its window, and presented a first frame."
