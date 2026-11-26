[CmdletBinding()]
param(
    [ValidateRange(0, 100)][double]$CoverageFloor = 1,
    [switch]$DiscoverWorkspaces,
    [switch]$InstallCoverageTool,
    [switch]$PrepareToolchains,
    [switch]$Run
)

$ErrorActionPreference = "Stop"

$selectedModes = @($DiscoverWorkspaces, $InstallCoverageTool, $PrepareToolchains, $Run) | Where-Object { $_ }
if ($selectedModes.Count -ne 1) {
    throw "Select exactly one operation."
}

$repoRootOutput = & git rev-parse --show-toplevel
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($repoRootOutput)) {
    throw "Could not resolve the repository root."
}
$repoRoot = [IO.Path]::GetFullPath($repoRootOutput.Trim())
$outputDirectory = Join-Path $repoRoot "_rust_ci"

function Write-GitHubOutput([string]$name, [string]$value) {
    if ($env:GITHUB_OUTPUT) {
        Add-Content -LiteralPath $env:GITHUB_OUTPUT -Value "$name=$value" -Encoding utf8
    }
}

function Write-GitHubMultilineOutput([string]$name, [string[]]$values) {
    if (-not $env:GITHUB_OUTPUT) { return }
    Add-Content -LiteralPath $env:GITHUB_OUTPUT -Value "$name<<RUST_CI_EOF" -Encoding utf8
    foreach ($value in $values) {
        Add-Content -LiteralPath $env:GITHUB_OUTPUT -Value $value -Encoding utf8
    }
    Add-Content -LiteralPath $env:GITHUB_OUTPUT -Value "RUST_CI_EOF" -Encoding utf8
}

function Get-TrackedManifests {
    $paths = @(& git -C $repoRoot ls-files -- "Cargo.toml" ":(glob)**/Cargo.toml")
    if ($LASTEXITCODE -ne 0) { throw "Could not list tracked Cargo manifests." }
    return @($paths | ForEach-Object { ([string]$_).Trim() } | Where-Object { $_ } | Sort-Object -Unique)
}

function Get-ManifestPaths {
    $manifests = Get-TrackedManifests
    return @($manifests | ForEach-Object { [IO.Path]::GetFullPath((Join-Path $repoRoot $_)) })
}

function Get-RepoRelativePath([string]$absolutePath) {
    $absolute = [IO.Path]::GetFullPath($absolutePath)
    $relative = [IO.Path]::GetRelativePath($repoRoot, $absolute).Replace("\", "/")
    if ($relative -eq ".." -or $relative.StartsWith("../", [StringComparison]::Ordinal)) {
        throw "Path is outside the repository: $absolutePath"
    }
    return $relative
}

function Get-WorkspaceMetadata {
    $workspacesByRoot = [System.Collections.Generic.Dictionary[string, object]]::new(
        [StringComparer]::OrdinalIgnoreCase
    )

    foreach ($manifestPath in (Get-ManifestPaths)) {
        $manifestDirectory = Split-Path -Parent $manifestPath
        Push-Location $manifestDirectory
        try {
            $metadataLines = & cargo metadata --locked --no-deps --format-version 1 --manifest-path $manifestPath
            $metadataExitCode = $LASTEXITCODE
            if ($metadataExitCode -ne 0) {
                throw "cargo metadata failed for '$manifestPath' with exit code $metadataExitCode."
            }
        } finally {
            Pop-Location
        }

        try {
            $metadata = ($metadataLines -join "`n") | ConvertFrom-Json
        } catch {
            throw "cargo metadata returned invalid JSON for '$manifestPath': $_"
        }

        $workspaceRoot = [IO.Path]::GetFullPath([string]$metadata.workspace_root)
        if (-not $workspacesByRoot.ContainsKey($workspaceRoot)) {
            $memberIds = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
            foreach ($memberId in $metadata.workspace_members) { [void]$memberIds.Add([string]$memberId) }
            $packages = @($metadata.packages | Where-Object { $memberIds.Contains([string]$_.id) })
            if ($packages.Count -eq 0) { continue }
            $workspacesByRoot.Add($workspaceRoot, [pscustomobject]@{
                Packages = $packages
                Root = $workspaceRoot
            })
        }
    }

    return @($workspacesByRoot.Values | Sort-Object { $_.Root })
}

function Invoke-NativeCommand([string]$workingDirectory, [string]$executable, [string[]]$arguments) {
    Push-Location $workingDirectory
    try {
        Write-Host "`n> $executable $($arguments -join ' ')"
        & $executable @arguments
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0) {
            throw "$executable $($arguments -join ' ') failed with exit code $exitCode."
        }
    } finally {
        Pop-Location
    }
}

function Get-SourceExclusionReason([string]$relativePath) {
    $excludedDirectories = @(
        "benches", "examples", "fixtures", "generated", "target", "tests",
        "third-party", "third_party", "vendor"
    )
    $segments = $relativePath.Replace("\", "/").Split("/")
    foreach ($segment in $segments) {
        if ($excludedDirectories -contains $segment.ToLowerInvariant()) { return "'$segment' directory" }
    }
    if ([IO.Path]::GetFileName($relativePath) -match "(?i)(^|[-_])generated\.rs$") { return "generated file name" }
    if ([IO.Path]::GetFileName($relativePath) -eq "tests.rs") { return "tests.rs unit-test module" }

    $sourcePath = Join-Path $repoRoot $relativePath
    if (Test-Path -LiteralPath $sourcePath) {
        $header = @(Get-Content -LiteralPath $sourcePath -TotalCount 8) -join "`n"
        if ($header -match "@generated") { return "@generated header" }
    }
    return $null
}

function Assert-UnitTestModuleConvention([string]$relativePath) {
    $sourceLines = @(Get-Content -LiteralPath (Join-Path $repoRoot $relativePath))
    $cfgTest = '^\s*#\[\s*cfg\s*\(\s*test\s*\)\s*\]\s*'
    $testsModule = '^(?:pub\b(?:\s*\([^)]*\))?\s*)?mod\s+tests\s*;\s*(?://.*)?$'
    for ($index = 0; $index -lt $sourceLines.Count; $index++) {
        $line = [string]$sourceLines[$index]
        $previous = if ($index -gt 0) { [string]$sourceLines[$index - 1] } else { "" }
        $next = if ($index + 1 -lt $sourceLines.Count) { ([string]$sourceLines[$index + 1]).Trim() } else { "" }
        $violation = if ($line -match $cfgTest) {
            $item = $line -replace $cfgTest, ""
            if (-not $item) { $item = $next }
            $item -notmatch $testsModule
        } else {
            $line.Trim() -match $testsModule -and $previous -notmatch "$cfgTest`$"
        }
        if ($violation) {
            throw ("{0}:{1}: Rust unit tests must live in a tests.rs module declared as '#[cfg(test)] mod tests;'." -f
                $relativePath, ($index + 1))
        }
    }
}

function Get-ProductionFiles($workspace) {
    $trackedRustFiles = @(& git -C $repoRoot ls-files -- "*.rs" ":(glob)**/*.rs")
    if ($LASTEXITCODE -ne 0) { throw "Could not list tracked Rust sources." }
    $trackedSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in $trackedRustFiles) {
        [void]$trackedSet.Add(([string]$path).Replace("\", "/"))
    }

    $paths = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $packageRoots = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $excludedSources = [System.Collections.Generic.SortedDictionary[string, string]]::new(
        [StringComparer]::OrdinalIgnoreCase
    )
    $hasProductionPackage = $false

    foreach ($package in $workspace.Packages) {
        $packageRoot = [IO.Path]::GetDirectoryName([string]$package.manifest_path)
        $packageRelativeRoot = Get-RepoRelativePath $packageRoot
        [void]$packageRoots.Add($packageRelativeRoot)
        $productionTargets = @($package.targets | Where-Object {
            $kind = @($_.kind)
            @("bin", "cdylib", "dylib", "lib", "proc-macro", "rlib", "staticlib") |
                Where-Object { $kind -contains $_ }
        })
        if ($productionTargets.Count -eq 0) { continue }
        $hasProductionPackage = $true

        foreach ($target in $productionTargets) {
            $targetPath = Get-RepoRelativePath ([string]$target.src_path)
            $exclusionReason = Get-SourceExclusionReason $targetPath
            if ($exclusionReason) {
                $excludedSources[$targetPath] = $exclusionReason
                continue
            }
            if (-not $trackedSet.Contains($targetPath)) {
                throw "Production Cargo target is not tracked: $targetPath"
            }
            [void]$paths.Add($targetPath)
        }

        foreach ($trackedPath in $trackedSet) {
            if ($packageRelativeRoot -ne "." -and
                -not $trackedPath.StartsWith("$packageRelativeRoot/", [StringComparison]::OrdinalIgnoreCase)) {
                continue
            }
            $exclusionReason = Get-SourceExclusionReason $trackedPath
            if ($exclusionReason) {
                $excludedSources[$trackedPath] = $exclusionReason
                continue
            }
            [void]$paths.Add($trackedPath)
        }
    }

    $files = @($paths | Sort-Object)
    foreach ($path in $files) { Assert-UnitTestModuleConvention $path }
    $excludedSourceEntries = @($excludedSources.GetEnumerator() | Where-Object { -not $paths.Contains($_.Key) } |
        ForEach-Object { [pscustomobject]@{ path = $_.Key; reason = $_.Value } })

    return [pscustomobject]@{
        ExcludedSources = $excludedSourceEntries
        Files = $files
        HasProductionPackage = $hasProductionPackage
        PackageRoots = @($packageRoots | Sort-Object)
    }
}

function Get-ProductionSourceKey(
    [string]$filename,
    $workspace,
    [System.Collections.Generic.HashSet[string]]$workspaceFiles
) {
    $normalizedName = $filename.Replace("\", "/")
    $candidatePaths = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $workspaceRoot = Join-Path $repoRoot ([string]$workspace.root)

    if ([IO.Path]::IsPathRooted($filename)) {
        $absolute = [IO.Path]::GetFullPath($filename)
        try {
            [void]$candidatePaths.Add((Get-RepoRelativePath $absolute))
        } catch {
            return $null
        }
    } else {
        try { [void]$candidatePaths.Add((Get-RepoRelativePath (Join-Path $repoRoot $normalizedName))) } catch {}
        try { [void]$candidatePaths.Add((Get-RepoRelativePath (Join-Path $workspaceRoot $normalizedName))) } catch {}
        foreach ($packageRoot in $workspace.packageRoots) {
            $packagePath = Join-Path $repoRoot ([string]$packageRoot)
            try { [void]$candidatePaths.Add((Get-RepoRelativePath (Join-Path $packagePath $normalizedName))) } catch {}
        }
    }

    $matches = @($candidatePaths | Where-Object { $workspaceFiles.Contains($_) })
    if ($matches.Count -gt 1) {
        throw "Cobertura path '$filename' maps to multiple production sources: $($matches -join ', ')."
    }
    if ($matches.Count -eq 1) { return $matches[0] }

    foreach ($candidate in $candidatePaths) {
        foreach ($packageRoot in $workspace.packageRoots) {
            $insidePackage = if ($packageRoot -eq ".") {
                $candidate -ne "."
            } else {
                $candidate.StartsWith("$packageRoot/", [StringComparison]::OrdinalIgnoreCase)
            }
            if ($insidePackage -and
                -not (Get-SourceExclusionReason $candidate)) {
                throw "Cobertura production source is not in the tracked source map: $filename ($candidate)."
            }
        }
    }
    return $null
}

function Normalize-CoberturaReport($workspace, $sourceInfo, [string]$reportPath) {
    if (-not (Test-Path -LiteralPath $reportPath)) {
        throw "Cobertura report is missing for workspace '$($workspace.root)': $reportPath"
    }
    if ((Get-Item -LiteralPath $reportPath).Length -eq 0) {
        throw "Cobertura report is empty for workspace '$($workspace.root)': $reportPath"
    }
    try {
        [xml]$report = Get-Content -LiteralPath $reportPath -Raw
    } catch {
        throw "Cobertura report is invalid for workspace '$($workspace.root)': $_"
    }

    $workspaceFiles = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $sourceInfo.Files) { [void]$workspaceFiles.Add([string]$file) }

    $workspaceContext = [pscustomobject]@{
        packageRoots = @($sourceInfo.PackageRoots)
        root = [string]$workspace.root
    }
    $files = @{}
    $classes = @($report.SelectNodes("/coverage/packages/package/classes/class"))
    if ($workspace.hasProductionPackage -and $classes.Count -eq 0) {
        throw "Cobertura report for workspace '$($workspace.root)' contains no classes."
    }
    foreach ($class in $classes) {
        $key = Get-ProductionSourceKey ([string]$class.GetAttribute("filename")) $workspaceContext $workspaceFiles
        if (-not $key) { continue }

        foreach ($line in $class.SelectNodes("./lines/line")) {
            $hits = 0L
            $lineNumber = 0
            if (-not [int]::TryParse($line.GetAttribute("number"), [ref]$lineNumber) -or $lineNumber -lt 1) {
                throw "Invalid Cobertura line number in '$reportPath'."
            }
            if (-not [long]::TryParse($line.GetAttribute("hits"), [ref]$hits) -or $hits -lt 0) {
                throw "Invalid Cobertura hit count for ${key}:$lineNumber in '$reportPath'."
            }
            if (-not $files.ContainsKey($key)) { $files[$key] = @{} }
            $files[$key][$lineNumber] += $hits
        }
    }

    if ($workspace.hasProductionPackage -and $files.Count -eq 0) {
        throw "Cobertura report for production workspace '$($workspace.root)' contains no production lines."
    }

    # Emit only line coverage. Raw method, package and document totals include excluded test code.
    [xml]$production = @"
<coverage branch-rate="0" branches-covered="0" branches-valid="0" complexity="0">
  <sources><source>.</source></sources>
  <packages><package branch-rate="0" complexity="0"><classes/></package></packages>
</coverage>
"@
    $covered = 0
    $valid = 0
    $coverage = $production.DocumentElement
    $package = $coverage.SelectSingleNode("packages/package")
    $classList = $package.SelectSingleNode("classes")
    $package.SetAttribute("name", [string]$workspace.root)
    foreach ($attribute in @("timestamp", "version")) {
        if ($report.DocumentElement.HasAttribute($attribute)) {
            $coverage.SetAttribute($attribute, $report.DocumentElement.GetAttribute($attribute))
        }
    }
    foreach ($key in ($files.Keys | Sort-Object)) {
        $class = $classList.AppendChild($production.CreateElement("class"))
        $lineHits = $files[$key]
        $classCovered = @($lineHits.Values | Where-Object { $_ -gt 0 }).Count
        $class.SetAttribute("branch-rate", "0")
        $class.SetAttribute("complexity", "0")
        $class.SetAttribute("filename", $key)
        $class.SetAttribute("line-rate", [Xml.XmlConvert]::ToString([double]($classCovered / $lineHits.Count)))
        $class.SetAttribute("name", $key)
        [void]$class.AppendChild($production.CreateElement("methods"))
        $lines = $class.AppendChild($production.CreateElement("lines"))
        foreach ($number in ($lineHits.Keys | Sort-Object)) {
            $line = $lines.AppendChild($production.CreateElement("line"))
            $line.SetAttribute("hits", [string]$lineHits[$number])
            $line.SetAttribute("number", [string]$number)
        }
        $covered += $classCovered
        $valid += $lineHits.Count
    }
    $rate = if ($valid) { [Xml.XmlConvert]::ToString([double]($covered / $valid)) } else { "0" }
    $coverage.SetAttribute("line-rate", $rate)
    $coverage.SetAttribute("lines-covered", [string]$covered)
    $coverage.SetAttribute("lines-valid", [string]$valid)
    $package.SetAttribute("line-rate", $rate)
    $production.Save($reportPath)
    $workspace | Add-Member -NotePropertyName "covered" -NotePropertyValue $covered
    $workspace | Add-Member -NotePropertyName "coverable" -NotePropertyValue $valid
    $workspace | Add-Member -NotePropertyName "sources" -NotePropertyValue @($files.Keys)
}

function Write-CoverageSummary($workspaces) {
    $totalCovered = 0
    $totalCoverable = 0
    $workspacesWithProduction = 0
    $sourceOwners = [System.Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)

    foreach ($workspace in $workspaces) {
        foreach ($source in $workspace.sources) {
            if ($sourceOwners.ContainsKey($source)) {
                throw ("Rust production source '{0}' is measured by workspaces '{1}' and '{2}'." -f `
                    $source, $sourceOwners[$source], $workspace.root)
            }
            $sourceOwners[$source] = $workspace.root
        }
        if ($workspace.hasProductionPackage) { $workspacesWithProduction++ }
        $totalCovered += $workspace.covered
        $totalCoverable += $workspace.coverable
        Write-Host "Workspace $($workspace.root): production lines $($workspace.covered)/$($workspace.coverable)"
        foreach ($excludedSource in $workspace.excludedSources) {
            Write-Host "  Excluded from production coverage: $($excludedSource.path) ($($excludedSource.reason))"
        }
    }

    if ($workspacesWithProduction -eq 0) {
        Write-Host "No production Rust crates were found; a coverage floor is not required."
        return
    }
    $coveragePercent = ($totalCovered / $totalCoverable) * 100.0
    Write-Host ("Production Rust line coverage: {0}/{1} ({2}%)" -f `
        $totalCovered, $totalCoverable, [math]::Round($coveragePercent, 3))
    if ($env:GITHUB_STEP_SUMMARY) {
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value "### Rust production line coverage" -Encoding utf8
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value "" -Encoding utf8
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY `
            -Value "| Covered production lines | Coverable production lines | Coverage |" -Encoding utf8
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value "| ---: | ---: | ---: |" -Encoding utf8
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value `
            ("| {0} | {1} | {2}% |" -f $totalCovered, $totalCoverable, [math]::Round($coveragePercent, 3)) `
            -Encoding utf8
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value "" -Encoding utf8
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY `
            -Value "| Workspace | Covered production lines | Coverable production lines | Coverage |" -Encoding utf8
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value "| --- | ---: | ---: | ---: |" -Encoding utf8
        foreach ($workspace in $workspaces) {
            $workspaceCoverage = if ($workspace.coverable -gt 0) {
                [math]::Round(($workspace.covered / $workspace.coverable) * 100.0, 3)
            } else {
                "n/a"
            }
            Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value `
                ("| {0} | {1} | {2} | {3} |" -f `
                    $workspace.root, $workspace.covered, $workspace.coverable, $workspaceCoverage) `
                -Encoding utf8
        }
        $excludedRows = @(foreach ($workspace in $workspaces) {
            foreach ($excludedSource in $workspace.excludedSources) {
                "| {0} | {1} | {2} |" -f $workspace.root, $excludedSource.path, $excludedSource.reason
            }
        })
        if ($excludedRows.Count -gt 0) {
            Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value @(
                "",
                "### Rust sources excluded from production coverage",
                "",
                "| Workspace | Source | Reason |",
                "| --- | --- | --- |"
            ) -Encoding utf8
            Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value $excludedRows -Encoding utf8
        }
    }

    if ($coveragePercent -lt $CoverageFloor) {
        throw ("Production Rust coverage {0}% is below the committed floor {1}%." -f `
            [math]::Round($coveragePercent, 3), $CoverageFloor)
    }
}

if ($PrepareToolchains) {
    $manifestPaths = @(Get-ManifestPaths)
    if ($manifestPaths.Count -eq 0) {
        Write-Host "No tracked Cargo manifests; Rust checks and coverage are not applicable."
        exit 0
    }

    foreach ($manifestPath in $manifestPaths) {
        $manifestDirectory = Split-Path -Parent $manifestPath
        Invoke-NativeCommand $manifestDirectory "rustup" @("toolchain", "install", "--no-self-update")
        Invoke-NativeCommand $manifestDirectory "rustup" @(
            "component", "add", "clippy", "llvm-tools-preview", "rustfmt"
        )
    }
    exit 0
}

if ($DiscoverWorkspaces) {
    $workspaces = @(Get-WorkspaceMetadata)
    if ($workspaces.Count -eq 0) {
        Write-Host "No Cargo workspace packages remain after metadata; checks and coverage are not applicable."
        Write-GitHubOutput "has-workspaces" "false"
        exit 0
    }
    $cacheWorkspaces = @($workspaces | ForEach-Object {
        Get-RepoRelativePath $_.Root
    })
    Write-Host "Discovered $($workspaces.Count) Cargo workspace root(s):"
    foreach ($workspace in $workspaces) { Write-Host "  $(Get-RepoRelativePath $workspace.Root)" }
    Write-GitHubOutput "has-workspaces" "true"
    Write-GitHubMultilineOutput "cache-workspaces" $cacheWorkspaces
    exit 0
}

if ($InstallCoverageTool) {
    $manifestPaths = @(Get-ManifestPaths)
    Invoke-NativeCommand (Split-Path -Parent $manifestPaths[0]) "cargo" @(
        "install", "cargo-llvm-cov", "--version", "0.9.2", "--locked"
    )
    exit 0
}

if ($Run) {
    $workspaces = @(Get-WorkspaceMetadata)
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

    $workspaceEntries = [System.Collections.Generic.List[object]]::new()

    for ($index = 0; $index -lt $workspaces.Count; $index++) {
        $workspace = $workspaces[$index]
        $workspaceId = "workspace-{0:D3}" -f ($index + 1)
        $relativeRoot = Get-RepoRelativePath $workspace.Root
        $sourceInfo = Get-ProductionFiles $workspace

        $entry = [pscustomobject]@{
            excludedSources = $sourceInfo.ExcludedSources
            hasProductionPackage = $sourceInfo.HasProductionPackage
            report = "_rust_ci/$workspaceId.cobertura.xml"
            root = $relativeRoot
        }
        $workspaceEntries.Add($entry)

        Invoke-NativeCommand $workspace.Root "cargo" @("fmt", "--all", "--", "--check")
        Invoke-NativeCommand $workspace.Root "cargo" @(
            "clippy", "--workspace", "--all-targets", "--locked", "--", "-D", "warnings"
        )
        Invoke-NativeCommand $workspace.Root "cargo" @("build", "--workspace", "--locked")
        Invoke-NativeCommand $workspace.Root "cargo" @("build", "--workspace", "--release", "--locked")
        Invoke-NativeCommand $workspace.Root "cargo" @(
            "test", "--workspace", "--locked", "--no-fail-fast"
        )

        if ($workspace.Packages.Count -gt 1) {
            foreach ($package in $workspace.Packages) {
                Invoke-NativeCommand $workspace.Root "cargo" @(
                    "test", "--manifest-path", [string]$package.manifest_path,
                    "--package", [string]$package.name,
                    "--locked", "--no-fail-fast"
                )
            }
        }

        $reportPath = Join-Path $repoRoot $entry.report
        Invoke-NativeCommand $workspace.Root "cargo" @(
            "llvm-cov", "--workspace", "--all-targets", "--locked", "--cobertura",
            "--output-path", $reportPath
        )
        Normalize-CoberturaReport $entry $sourceInfo $reportPath
    }

    Write-GitHubMultilineOutput "reports" @($workspaceEntries | ForEach-Object { $_.report })
    Write-CoverageSummary $workspaceEntries
    exit 0
}
