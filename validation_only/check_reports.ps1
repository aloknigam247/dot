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
    $env:DOT_RUST_COVERAGE_FLOOR = "100"
    Assert-Rejection "below measured floor" "is below the committed floor" { Write-CoverageSummary $entries }
    Remove-Item -LiteralPath $samplePath
    Assert-Rejection "missing one workspace report" "missing for workspace '$($sample.root)'" {
        Normalize-CoberturaReport $sample $sourceInfo $samplePath
    }
    Assert-Rejection "other src/lib.rs cannot replace missing report" "missing for workspace '$($sample.root)'" {
        $fresh = @($entries | Select-Object hasProductionPackage,report,root)
        Write-CoverageSummary $fresh
    }
    [IO.File]::WriteAllText($samplePath, "")
    Assert-Rejection "empty report" "empty for workspace" { Normalize-CoberturaReport $sample $sourceInfo $samplePath }
    [IO.File]::WriteAllText($samplePath, "<coverage><packages/></coverage>")
    Assert-Rejection "no mapped production classes" "contains no classes" {
        Normalize-CoberturaReport $sample $sourceInfo $samplePath
    }
    $badPath = "$($sample.root)/src/not_tracked.rs"
    $xml = "<coverage><packages><package><classes><class filename='$badPath'><lines><line number='1' hits='1'/>" +
        "</lines></class></classes></package></packages></coverage>"
    [IO.File]::WriteAllText($samplePath, $xml)
    Assert-Rejection "unmapped production source" "not in the tracked source map" {
        Normalize-CoberturaReport $sample $sourceInfo $samplePath
    }
    $ambiguous = [pscustomobject]@{ root = "rust_fixture/multi"; packageRoots = @("rust_fixture/multi/left",
        "rust_fixture/multi/members/right") }
    $sources = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $excluded = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($root in $ambiguous.packageRoots) { [void]$sources.Add("$root/src/lib.rs") }
    Assert-Rejection "ambiguous source" "maps to multiple production sources" {
        Get-ProductionSourceKey "src/lib.rs" $ambiguous $sources $excluded
    }
} finally {
    foreach ($entry in $entries) {
        $reportPath = Join-Path $repoRoot $entry.report
        if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath }
    }
    Remove-Item -LiteralPath $directory
}
