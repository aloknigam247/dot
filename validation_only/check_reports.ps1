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
        $root = Get-RepoRelativePath $workspaces[$i].Root
        $copy = Join-Path $repoRoot $relativeReport
        [xml]$raw = Get-Content -Raw -LiteralPath $reportFiles[$i].FullName
        foreach ($class in $raw.SelectNodes("//class")) {
            $filename = $class.GetAttribute("filename")
            if ($filename.StartsWith("$root/", [StringComparison]::Ordinal)) {
                $class.SetAttribute("filename", $filename.Substring($root.Length + 1).Replace("/", "\"))
            }
        }
        $raw.Save($copy)
        [pscustomobject]@{
            hasProductionPackage = $true
            report = $relativeReport
            root = $root
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
    $badPath = "src\not_tracked.rs"
    $untrackedSource = "<coverage><packages><package><classes><class filename='$badPath'><lines><line number='1' hits='1'/>" +
        "</lines></class></classes></package></packages></coverage>"
    [IO.File]::WriteAllText($samplePath, $untrackedSource)
    Assert-Rejection "unmapped production source" "not in the tracked source map" {
        Normalize-CoberturaReport $sample $sourceInfo $samplePath
    }
    $multi = [pscustomobject]@{ root = "rust_fixture/multi"; packageRoots = @("rust_fixture/multi/left",
        "rust_fixture/multi/members/right") }
    $sources = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($root in $multi.packageRoots) { [void]$sources.Add("$root/src/lib.rs") }
    $resolved = Get-ProductionSourceKey "left\src\lib.rs" $multi $sources
    if ($resolved -cne "rust_fixture/multi/left/src/lib.rs") { throw "Relative source resolved to '$resolved'." }
    Write-Host "Verified relative Cobertura source resolves against the workspace root: $resolved"
    $exclusionCases = [ordered]@{
        "crates/a/src/tests/helpers.rs" = "'tests' directory"
        "crates/a/tests/it.rs" = "'tests' directory"
        "crates/tests/src/lib.rs" = $null
        "examples/demo.rs" = "'examples' directory"
        "fixtures/sample.rs" = $null
        "src/session/tests.rs" = "tests.rs unit-test module"
        "src/tests_util.rs" = $null
    }
    foreach ($path in $exclusionCases.Keys) {
        $reason = Get-SourceExclusionReason $path @(".", "crates/a", "crates/tests")
        if ($reason -cne $exclusionCases[$path]) { throw "Exclusion for '$path' was '$reason'." }
    }
    Write-Host "Verified innermost-package exclusions for $($exclusionCases.Count) paths."
    $conventionPath = "rust_fixture/standalone/src/convention_probe.rs"
    $conventionFile = Join-Path $repoRoot $conventionPath
    $conventionCases = [ordered]@{
        "all(test) predicate" = @("#[cfg(all(test, windows))]`nmod x {}`n", "1: test cfg predicate")
        "cfg_attr(test)" = @("#[cfg_attr(test, derive(Debug))]`nstruct S;`n", "1: test cfg predicate")
        "cfg(test) before another attribute" = @("#[cfg(test)]`n#[path = `"x.rs`"]`nmod tests;`n",
            "1: test cfg predicate")
        "inline test module" = @("#[cfg(test)]`nmod inline_checks {}`n", "1: test cfg predicate")
        "inline tests module" = @("#[cfg(test)]`nmod tests {}`n", "2: tests module not declared")
        "inner cfg(test)" = @("#![cfg(test)]`nmod tests;`n", "1: test cfg predicate")
        "multi-line cfg" = @("#[cfg(`n    any(unix, test)`n)]`nfn f() {}`n", "1: test cfg predicate")
        "not(test) predicate" = @("#[cfg(not(test))]`nfn f() {}`n", "1: test cfg predicate")
        "same-line inline test item" = @("#[cfg(test)] fn helper() {}`n", "1: test cfg predicate")
        "test attribute" = @("fn f() {}`n#[test]`nfn t() {}`n", "2: test attribute")
        "tokio test attribute" = @("#[tokio::test(flavor = `"multi_thread`")]`nasync fn t() {}`n",
            "1: test attribute")
        "unconditional tests module" = @("mod tests;`n", "1: tests module not declared")
    }
    foreach ($case in $conventionCases.Keys) {
        [IO.File]::WriteAllText($conventionFile, $conventionCases[$case][0])
        Assert-Rejection $case "${conventionPath}:$($conventionCases[$case][1])" {
            Assert-UnitTestModuleConvention $conventionPath
        }
    }
    $ignoredText = @(
        "// #[test]",
        "/* #[cfg(test)] /* nested */ mod tests { */",
        "const S: &str = `"#[test] mod tests {`";",
        "const R: &str = r#`"#[cfg(test)] `" mod tests {`"#;",
        "const C: char = '`"';",
        "fn f<'a>(x: &'a str) -> &'a str { x }",
        "#[cfg(feature = `"test`")]",
        "fn g() {}",
        "#[test_case]",
        "fn h() {}",
        "#[cfg(test)]",
        "pub(crate) mod tests;"
    ) -join "`n"
    foreach ($accepted in @("#[cfg(test)]`nmod tests;`n", "pub fn f() {}`n#[cfg(test)] mod tests; // unit`n",
        $ignoredText)) {
        [IO.File]::WriteAllText($conventionFile, $accepted)
        Assert-UnitTestModuleConvention $conventionPath
    }
    Write-Host "Verified accepted tests.rs module declarations, comments, and literals."
    Remove-Item -LiteralPath $conventionFile
} finally {
    foreach ($entry in $entries) {
        $reportPath = Join-Path $repoRoot $entry.report
        if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath }
    }
    Remove-Item -LiteralPath $directory
}
