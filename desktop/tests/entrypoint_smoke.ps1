param(
    [Parameter(Mandatory)][string]$DriverPath,
    [Parameter(Mandatory)][string]$ExePath
)

$ErrorActionPreference = "Stop"

$inputText = "dot entrypoint stdin"
$id = [guid]::NewGuid().ToString()
$scratch = Join-Path ([IO.Path]::GetTempPath()) "dot_entrypoint_$id"
$cmdlinePath = Join-Path $scratch "cmdline_$id.txt"
$framePath = Join-Path $scratch "frame_$id.txt"
$initPath = Join-Path $scratch "init_$id.lua"
$outputPath = Join-Path $scratch "buffer_$id.txt"

function Invoke-Entrypoint {
    param(
        [string[]]$Arguments,
        [string]$InputText = "",
        [string]$Path
    )

    $started = $false
    $process = [Diagnostics.Process]::new()
    $startInfo = [Diagnostics.ProcessStartInfo]::new($Path)
    foreach ($argument in $Arguments) {
        $startInfo.ArgumentList.Add($argument)
    }
    $startInfo.CreateNoWindow = $true
    $startInfo.Environment["DOT_BOOT_PROFILE"] = "1"
    [void]$startInfo.Environment.Remove("DOT_BOOT_PROFILE_FILE")
    $startInfo.Environment["DOT_PROFILE_PAINT"] = "1"
    $startInfo.Environment["DOT_TEST_CMDLINE"] = $cmdlinePath
    $startInfo.Environment["DOT_TEST_FRAME"] = $framePath
    $startInfo.Environment["DOT_TEST_OUTPUT"] = $outputPath
    $startInfo.Environment["QML_DISABLE_DISK_CACHE"] = "1"
    $startInfo.Environment["QT_FORCE_STDERR_LOGGING"] = "1"
    $startInfo.Environment["QT_LOGGING_RULES"] = "default.debug=true"
    $startInfo.Environment["QT_MESSAGE_PATTERN"] = "%{message}"
    $startInfo.Environment["QT_QPA_PLATFORM"] = "minimal"
    $startInfo.Environment["QT_QUICK_BACKEND"] = "software"
    foreach ($name in @("XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME")) {
        $startInfo.Environment[$name] = $scratch
    }
    $startInfo.RedirectStandardError = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.UseShellExecute = $false
    $startInfo.WorkingDirectory = $scratch
    $process.StartInfo = $startInfo
    try {
        $started = $process.Start()
        if (-not $started) { throw "Could not start '$Path'." }
        $lineTask = $process.StandardError.ReadLineAsync()
        $stderr = [Text.StringBuilder]::new()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $process.StandardInput.Write($InputText)
        $process.StandardInput.Close()
        while ($true) {
            if ($watch.Elapsed -gt [TimeSpan]::FromSeconds(45)) {
                if (-not $process.HasExited) {
                    $process.Kill($true)
                    $process.WaitForExit()
                }
                [void]$stderr.AppendLine($lineTask.GetAwaiter().GetResult())
                [void]$stderr.Append($process.StandardError.ReadToEnd())
                $observed = if (Test-Path -LiteralPath $outputPath) {
                    [IO.File]::ReadAllText($outputPath)
                } else { "<no buffer sample>" }
                throw "Entrypoint timed out: '$Path'. Observed buffer: '$observed'.`n$stderr"
            }
            if (-not $lineTask.Wait(50)) { continue }
            $line = $lineTask.GetAwaiter().GetResult()
            if ($null -eq $line) {
                if ($process.WaitForExit(50)) { break }
                continue
            }
            [void]$stderr.AppendLine($line)
            if ($line -match "^\[boot\] first frame swapped ") {
                [IO.File]::WriteAllText($framePath, "ready")
            }
            $lineTask = $process.StandardError.ReadLineAsync()
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            throw "Entrypoint exited with $($process.ExitCode):`n$stderr"
        }
        return [pscustomobject]@{ Stderr = $stderr.ToString(); Stdout = $stdout }
    } finally {
        if ($started -and -not $process.HasExited) {
            $process.Kill($true)
            $process.WaitForExit()
        }
        $process.Dispose()
    }
}

if (Test-Path -LiteralPath $scratch) { throw "Test directory already exists: '$scratch'." }
[IO.Directory]::CreateDirectory($scratch) | Out-Null
try {
    [IO.File]::WriteAllText($initPath, @"
vim.opt.swapfile = false
vim.opt.guifont = "Consolas:h12"
local function ext_cmdline()
  return vim.api.nvim_list_uis()[1].ext_cmdline
end
local at_attach
vim.api.nvim_create_autocmd("UIEnter", { once = true, callback = function()
  at_attach = ext_cmdline()
end })
local previous
local timer = vim.uv.new_timer()
timer:start(50, 50, vim.schedule_wrap(function()
  if vim.fn.filereadable(vim.env.DOT_TEST_FRAME) ~= 1 then return end
  local lines = vim.api.nvim_buf_get_lines(0, 0, -1, false)
  local text = table.concat(lines, "\n")
  if text ~= previous then
    vim.fn.writefile(lines, vim.env.DOT_TEST_OUTPUT)
    previous = text
  end
  if text == "$inputText" then
    timer:stop()
    timer:close()
    vim.wait(5000, ext_cmdline)
    vim.fn.writefile({ tostring(at_attach), tostring(ext_cmdline()) }, vim.env.DOT_TEST_CMDLINE)
    vim.cmd("qa!")
  end
end))
"@.Replace("`r`n", "`n"))

    $help = Invoke-Entrypoint -Path $ExePath -Arguments @("--help")
    if ($help.Stdout -notmatch "^Usage: dot " -or $help.Stdout -notmatch "--dot-rounded_highlights") {
        throw "The executable did not print the expected dot help: $($help.Stdout)"
    }
    foreach ($case in @(
        @{ Args = @("--dot-ext_cmdline"); AtAttach = "true"; Name = "CLI" }
        @{ Args = @("--cmd", "let g:dot_ext_cmdline = v:true"); AtAttach = "false"; Name = "g:dot_ext_cmdline" }
    )) {
        $arguments = @("--clean", "-u", $initPath, "--dot-rounded_highlights=ErrorMsg") + $case.Args + "-"
        $run = Invoke-Entrypoint -Path $DriverPath -Arguments $arguments -InputText $inputText
        if ($run.Stdout -notmatch "(?m)^application=dot\r?$" -or
            $run.Stdout -notmatch "(?m)^organization=dot\r?$") {
            throw "Unexpected application identity: $($run.Stdout)"
        }
        if ($run.Stderr -notmatch "dot paint: [0-9]+ us") {
            throw "The paint profiling path did not run: $($run.Stderr)"
        }
        if ([IO.File]::ReadAllText($outputPath).TrimEnd() -ne $inputText) {
            throw "Neovim did not receive the redirected stdin."
        }
        $states = [IO.File]::ReadAllLines($cmdlinePath) -join ","
        if ($states -ne "$($case.AtAttach),true") {
            throw "$($case.Name): ext_cmdline (attach,startup): expected '$($case.AtAttach),true', got '$states'."
        }
        Remove-Item -LiteralPath $cmdlinePath, $framePath, $outputPath
    }
    Write-Host "PASS: dot help, identity, first frame, paint profiling, stdin and ext_cmdline config."
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force
}
