[CmdletBinding()]
param(
    [string[]]$GradleArgs = @(":chat_cli:nativeCompile", ":chat_cli:nativeSmokeTest"),
    [ValidatePattern("^[1-9][0-9]*[gm]$")]
    [string]$NativeHeap = "6g",
    [ValidateRange(1, 128)]
    [int]$NativeThreads = 2
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

function Stop-BuildTree([int]$ProcessId) {
    $children = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$ProcessId")
    foreach ($child in $children) { Stop-BuildTree $child.ProcessId }
    Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue
}

function Get-DirectoryBytes([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return 0 }
    (Get-ChildItem -LiteralPath $Path -File -Recurse | Measure-Object -Property Length -Sum).Sum
}

foreach ($arg in $GradleArgs) {
    if ($arg -notmatch "^[a-zA-Z0-9_:.\-=]+$") { throw "Unsupported Gradle argument: $arg" }
}
if (-not $env:JAVA_HOME -or -not (Test-Path -LiteralPath "$env:JAVA_HOME\bin\java.exe")) {
    throw "Preflight: JAVA_HOME must point to JDK 17."
}
$javaVersion = (& "$env:JAVA_HOME\bin\java.exe" -version 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $javaVersion -notmatch 'version "17\.') {
    throw "Preflight: JAVA_HOME must remain on JDK 17. Observed: $javaVersion"
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw "Preflight: vswhere is missing; install MSVC and Windows SDK." }
$vs = (& $vswhere -latest -products "*" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1)
if (-not $vs) { throw "Preflight: MSVC x64 build tools are missing." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) { throw "Preflight: vcvars64.bat is missing: $vcvars" }

$graal = & (Join-Path $PSScriptRoot "ensure_graalvm.ps1")
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$reports = Join-Path $root "chat_cli\build\reports\native"
New-Item -ItemType Directory -Force -Path $reports | Out-Null
$id = [guid]::NewGuid().ToString()
$commandPath = Join-Path $reports "native_build_$id.cmd"
$errorPath = Join-Path $reports "native_build_$id.stderr.log"
$logPath = Join-Path $reports "native_build_$id.log"
$reportPath = Join-Path $reports "native_build_$id.json"
$arguments = (@("--no-daemon", "--console=plain", "-Pdot.native.heap=$NativeHeap",
            "-Pdot.native.threads=$NativeThreads") + $GradleArgs) -join " "
$command = @"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 3
where cl.exe >nul 2>nul
if errorlevel 1 exit /b 3
where link.exe >nul 2>nul
if errorlevel 1 exit /b 3
if not defined WindowsSdkDir exit /b 3
if not exist "%WindowsSdkDir%Include\%WindowsSDKVersion%um\Windows.h" exit /b 3
set "GRAALVM_HOME=$($graal.Home)"
cd /d "$root"
call "$root\gradlew.bat" $arguments
exit /b %errorlevel%
"@
[IO.File]::WriteAllText($commandPath, $command, [Text.UTF8Encoding]::new($false))
$watch = [Diagnostics.Stopwatch]::StartNew()
$process = $null
$timedOut = $false
try {
    Write-Host "Native build log: $logPath"
    $process = Start-Process -FilePath $env:ComSpec -ArgumentList "/d /s /c `"`"$commandPath`"`"" `
        -RedirectStandardOutput $logPath -RedirectStandardError $errorPath -PassThru -NoNewWindow
    if (-not $process.WaitForExit([int][TimeSpan]::FromMinutes(30).TotalMilliseconds)) {
        $timedOut = $true
        Stop-BuildTree $process.Id
    }
    $process.WaitForExit()
    $process.Refresh()
    $output = (Get-Content -LiteralPath $logPath -Raw) + "`n" + (Get-Content -LiteralPath $errorPath -Raw)
    Write-Host (($output -split "\r?\n" | Select-Object -Last 100) -join "`n")
    if ($timedOut) { throw "Native build exceeded 30 minutes. Review resource limits before retrying." }
    if ($process.ExitCode -eq 3) { throw "Preflight: MSVC or Windows SDK setup failed. See $logPath" }
    if ($process.ExitCode -ne 0) { throw "Native build failed (exit $($process.ExitCode)). See $logPath and $errorPath" }
    $compiled = $output -match "(?m)^> Task :chat_cli:nativeCompile\s*$"
    $selected = [regex]::Matches($output, "(?im)^\[native-image-plugin\] Native Image executable path: (.+?)\s*$")
    if ($compiled -and -not ($selected | Where-Object { $_.Groups[1].Value -ieq $graal.NativeImage })) {
        throw "Preflight: native-image used by Gradle does not match $($graal.NativeImage). See $logPath"
    }
    $executable = Join-Path $root "chat_cli\build\native\nativeCompile\dot_chat.exe"
    if (-not (Test-Path -LiteralPath $executable)) { throw "Native build produced no dot_chat.exe." }
    Write-Host "Built $executable"
} finally {
    $watch.Stop()
    $exe = Join-Path $root "chat_cli\build\native\nativeCompile\dot_chat.exe"
    $log = if (Test-Path -LiteralPath $logPath) { Get-Content -LiteralPath $logPath -Raw } else { "" }
    $rss = [regex]::Match($log, "Peak RSS: ([0-9.,]+\s*[GMK]i?B)").Groups[1].Value
    [ordered]@{
        buildBytes = Get-DirectoryBytes (Join-Path $root "chat_cli\build")
        cacheBytes = Get-DirectoryBytes (Split-Path $graal.Archive)
        command = "gradlew.bat $arguments"
        executableBytes = if (Test-Path -LiteralPath $exe) { (Get-Item -LiteralPath $exe).Length } else { 0 }
        exitCode = if ($process -and $process.HasExited) { $process.ExitCode } else { $null }
        nativeImage = $graal.NativeImage
        peakRss = $rss
        stderr = $errorPath
        stdout = $logPath
        timedOut = $timedOut
        wallSeconds = $watch.Elapsed.TotalSeconds
    } | ConvertTo-Json | Set-Content -LiteralPath $reportPath
    Remove-Item -LiteralPath $commandPath -Force
    if ($process) { $process.Dispose() }
    Write-Host "Native build report: $reportPath"
}
