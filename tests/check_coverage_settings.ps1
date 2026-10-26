$ErrorActionPreference = "Stop"

$outPath = Join-Path ([IO.Path]::GetTempPath()) "dot_coverage_$([guid]::NewGuid()).runsettings"
$root = Split-Path -Parent $PSScriptRoot
$cases = @(
    @{ Included = $false; Path = "$root-backup\src\AppIcon.cpp" }
    @{ Included = $false; Path = "$root\build\dev\vcpkg_installed\x64-windows\include\msgpack.hpp" }
    @{ Included = $true; Path = "$root\include\AppIcon.h" }
    @{ Included = $false; Path = "$root\include_extra\AppIcon.h" }
    @{ Included = $true; Path = "$root\src\AppIcon.cpp" }
    @{ Included = $false; Path = "$root\src_extra\AppIcon.cpp" }
    @{ Included = $false; Path = "$root\tests\unit\test_app_icon.cpp" }
)

try {
    & (Join-Path $root "scripts\coverage_settings.ps1") -OutPath $outPath
    [xml]$settings = Get-Content -LiteralPath $outPath -Raw
    $filters = @($settings.SelectNodes("//CodeCoverage/Sources/Include/Source") | ForEach-Object { $_.InnerText })
    foreach ($case in $cases) {
        $included = [bool]($filters | Where-Object { $case.Path -match $_ })
        if ($included -ne $case.Included) {
            throw "Expected included=$($case.Included), got included=$included for $($case.Path)"
        }
    }
    Write-Host "PASS: coverage includes only this checkout's src and include trees."
} finally {
    if (Test-Path -LiteralPath $outPath) {
        Remove-Item -LiteralPath $outPath
    }
}
