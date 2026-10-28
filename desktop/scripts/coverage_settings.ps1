[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutPath
)

$ErrorActionPreference = "Stop"

$outFile = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutPath)
$root = Split-Path -Parent $PSScriptRoot
[xml]$settings = Get-Content -LiteralPath (Join-Path $root "cov.runsettings") -Raw
$sourceRoot = [regex]::Escape($root)

foreach ($source in $settings.SelectNodes("//CodeCoverage/Sources/Include/Source")) {
    $source.InnerText = $source.InnerText.Replace("@SOURCE_ROOT@", $sourceRoot)
}

[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($outFile)) | Out-Null
$settings.Save($outFile)
