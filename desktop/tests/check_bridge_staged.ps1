using namespace System.Reflection.PortableExecutable

[CmdletBinding()]
param([Parameter(Mandatory)][string]$DeployDir)

$ErrorActionPreference = "Stop"
$fix = "FIX: Build the dot target to build and stage the bridge."
$reader = $null
$stream = $null
$bridge = Join-Path $DeployDir "dot_copilot_bridge.exe"
foreach ($path in @((Join-Path $DeployDir "dot.exe"), $bridge)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing executable: $path`n$fix" }
}

try {
    $stream = [IO.File]::OpenRead($bridge)
    $reader = [PEReader]::new($stream)
    $headers = $reader.PEHeaders
    $coff = $headers.CoffHeader
    if ($null -eq $headers.PEHeader -or $coff.Machine -ne [Machine]::Amd64 -or
        -not $coff.Characteristics.HasFlag([Characteristics]::ExecutableImage) -or $headers.IsDll) {
        throw "dot_copilot_bridge.exe is not a Windows x64 executable: $bridge (machine $($coff.Machine))"
    }
} catch [BadImageFormatException] {
    throw "dot_copilot_bridge.exe is not a Windows x64 executable: $bridge ($($_.Exception.Message))`n$fix"
} catch {
    throw "$($_.Exception.Message)`n$fix"
} finally {
    if ($reader) { $reader.Dispose() }
    if ($stream) { $stream.Dispose() }
}
