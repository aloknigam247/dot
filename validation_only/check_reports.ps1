$ErrorActionPreference = "Stop"
$rootOutput = git rev-parse --show-toplevel
if ($LASTEXITCODE -ne 0) { throw "Cannot locate fixture repository." }
$repoRoot = [IO.Path]::GetFullPath($rootOutput)
$scriptFile = Join-Path $repoRoot "common\scripts\rust_ci.ps1"
$ast = [Management.Automation.Language.Parser]::ParseFile($scriptFile, [ref]$null, [ref]$null)
foreach ($def in $ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    . ([scriptblock]::Create($def.Extent.Text))
}

function Assert-Rejection([string]$case, [string]$diagnostic, [scriptblock]$action) {
    try { & $action } catch {
        if ($_.Exception.Message -notlike "*$diagnostic*") { throw }
        Write-Host "Verified ${case}: $($_.Exception.Message)"
        return
    }
    throw "Invalid fixture '$case' was accepted."
}

$directory = Join-Path $repoRoot "integrity-$([guid]::NewGuid())"
New-Item -ItemType Directory -Path $directory -ErrorAction Stop | Out-Null
$workspaces = @(Get-WorkspaceMetadata)
if ($workspaces.Count -ne 2) { throw "Expected two independent fixture reports." }
$reportFiles = @(Get-ChildItem (Join-Path $repoRoot "_rust_ci") -Filter "workspace-*.cobertura.xml" | Sort-Object Name)
if ($reportFiles.Count -ne 2) { throw "Expected two real coverage artifacts." }
$entries = @(
    for ($i = 0; $i -lt $workspaces.Count; $i++) {
        $relativeReport = Get-RepoRelativePath (Join-Path $directory $reportFiles[$i].Name)
        Copy-Item -LiteralPath $reportFiles[$i].FullName -Destination (Join-Path $repoRoot $relativeReport)
        [pscustomobject]@{
            hasProductionPackage = $true
            report = $relativeReport
            root = Get-RepoRelativePath $workspaces[$i].Root
        }
    }
)
$sample = $entries[1]
$samplePath = Join-Path $repoRoot $sample.report
$sourceInfo = Get-ProductionFiles $workspaces[1]
try {
    foreach ($i in 0..1) {
        $path = Join-Path $repoRoot $entries[$i].report
        $info = Get-ProductionFiles $workspaces[$i]
        Normalize-CoberturaReport $entries[$i] $info $path
        [xml]$xml = Get-Content -Raw -LiteralPath $path
        $lines = @($xml.SelectNodes("//class/lines/line"))
        $covered = @($lines | Where-Object { [int]$_.hits -gt 0 }).Count
        if ([int]$xml.coverage.'lines-valid' -ne $lines.Count -or [int]$xml.coverage.'lines-covered' -ne $covered) {
            throw "Published workspace totals differ from the actual production lines in $path."
        }
        if ([string]$xml.coverage.sources.source -cne "." -or $xml.SelectNodes("//method").Count) {
            throw "Report contains nonportable source roots or raw method metadata."
        }
        Write-Host "Verified normalized report $($entries[$i].root): $covered/$($lines.Count) production lines."
    }
    $CoverageFloor = 100
    Assert-Rejection "below measured floor" "is below the committed floor" { Write-CoverageSummary $entries }
    $overlap = [pscustomobject]@{
        coverable = 1
        covered = 1
        hasProductionPackage = $true
        root = "rust_fixture/overlap"
        sources = @(([string]$entries[0].sources[0]).ToUpperInvariant())
    }
    Assert-Rejection "source measured by two workspaces" "is measured by workspaces '$($entries[0].root)'" {
        Write-CoverageSummary @($entries[0], $overlap)
    }
    Remove-Item -LiteralPath $samplePath
    Assert-Rejection "missing one workspace report" "missing for workspace '$($sample.root)'" {
        Normalize-CoberturaReport $sample $sourceInfo $samplePath
    }
    [IO.File]::WriteAllText($samplePath, "")
    Assert-Rejection "empty report" "empty for workspace" { Normalize-CoberturaReport $sample $sourceInfo $samplePath }
    [IO.File]::WriteAllText($samplePath, "<coverage><packages/></coverage>")
    Assert-Rejection "no mapped production classes" "contains no classes" {
        Normalize-CoberturaReport $sample $sourceInfo $samplePath
    }
    $badPath = "$($sample.root)/src/not_tracked.rs"
    $untrackedSource = "<coverage><packages><package><classes><class filename='$badPath'><lines><line number='1' hits='1'/>" +
        "</lines></class></classes></package></packages></coverage>"
    [IO.File]::WriteAllText($samplePath, $untrackedSource)
    Assert-Rejection "unmapped production source" "not in the tracked source map" {
        Normalize-CoberturaReport $sample $sourceInfo $samplePath
    }
    $ambiguous = [pscustomobject]@{ root = "rust_fixture/multi"; packageRoots = @("rust_fixture/multi/left",
        "rust_fixture/multi/members/right") }
    $sources = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($root in $ambiguous.packageRoots) { [void]$sources.Add("$root/src/lib.rs") }
    Assert-Rejection "ambiguous source" "maps to multiple production sources" {
        Get-ProductionSourceKey "src/lib.rs" $ambiguous $sources
    }
    $conventionPath = "rust_fixture/standalone/src/convention_probe.rs"
    $conventionFile = Join-Path $repoRoot $conventionPath
    $conventionCases = [ordered]@{
        "inline test module" = "#[cfg(test)]`nmod inline_checks {}`n"
        "same-line inline test item" = "#[cfg(test)] fn helper() {}`n"
        "unconditional tests module" = "mod tests;`n"
    }
    foreach ($case in $conventionCases.Keys) {
        [IO.File]::WriteAllText($conventionFile, $conventionCases[$case])
        Assert-Rejection $case "${conventionPath}:1: Rust unit tests must live in a tests.rs module" {
            Assert-UnitTestModuleConvention $conventionPath
        }
    }
    foreach ($accepted in @("#[cfg(test)]`nmod tests;`n", "pub fn f() {}`n#[cfg(test)] mod tests; // unit`n")) {
        [IO.File]::WriteAllText($conventionFile, $accepted)
        Assert-UnitTestModuleConvention $conventionPath
    }
    Write-Host "Verified accepted tests.rs module declarations."
    Remove-Item -LiteralPath $conventionFile
} finally {
    foreach ($entry in $entries) {
        $reportPath = Join-Path $repoRoot $entry.report
        if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath }
    }
    Remove-Item -LiteralPath $directory
}
