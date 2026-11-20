[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Endpoint,
    [Parameter(Mandatory)]
    [string]$Executable,
    [Parameter(Mandatory)]
    [string]$OutputPath,
    [Parameter(Mandatory)]
    [string]$SignalPath
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public sealed class DotChatNativeConsole : IDisposable {
    [StructLayout(LayoutKind.Sequential)]
    struct SecurityAttributes {
        public int Length;
        public IntPtr Descriptor;
        [MarshalAs(UnmanagedType.Bool)] public bool Inherit;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct StartupInfo {
        public int Size;
        public string Reserved;
        public string Desktop;
        public string Title;
        public int X, Y, XSize, YSize, XChars, YChars, Fill, Flags;
        public short ShowWindow, ReservedSize;
        public IntPtr ReservedBytes, Input, Output, Error;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct ProcessInformation {
        public IntPtr Process, Thread;
        public int ProcessId, ThreadId;
    }
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool AttachConsole(int processId);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CreatePipe(out IntPtr read, out IntPtr write, ref SecurityAttributes security, int size);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcess(string application, StringBuilder command, IntPtr processSecurity,
        IntPtr threadSecurity, bool inherit, int flags, IntPtr environment, string directory,
        ref StartupInfo startup, out ProcessInformation information);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GenerateConsoleCtrlEvent(int signal, int group);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetHandleInformation(IntPtr handle, int mask, int flags);

    public StreamWriter Input { get; private set; }
    public StreamReader Output { get; private set; }
    public Process Process { get; private set; }

    static void Require(bool success, string operation) {
        if (!success) throw new Win32Exception(Marshal.GetLastWin32Error(), operation);
    }

    public DotChatNativeConsole(string executable) {
        IntPtr inputRead = IntPtr.Zero, inputWrite = IntPtr.Zero;
        IntPtr outputRead = IntPtr.Zero, outputWrite = IntPtr.Zero;
        var security = new SecurityAttributes {
            Length = Marshal.SizeOf<SecurityAttributes>(), Inherit = true
        };
        try {
            Require(CreatePipe(out inputRead, out inputWrite, ref security, 0), "CreatePipe(stdin)");
            Require(CreatePipe(out outputRead, out outputWrite, ref security, 0), "CreatePipe(stdout)");
            Require(SetHandleInformation(inputWrite, 1, 0), "SetHandleInformation(stdin)");
            Require(SetHandleInformation(outputRead, 1, 0), "SetHandleInformation(stdout)");
            var startup = new StartupInfo {
                Size = Marshal.SizeOf<StartupInfo>(), Flags = 0x101,
                Input = inputRead, Output = outputWrite, Error = outputWrite
            };
            ProcessInformation information;
            Require(CreateProcess(executable, new StringBuilder("\"" + executable + "\""), IntPtr.Zero,
                IntPtr.Zero, true, 0x10, IntPtr.Zero, Path.GetDirectoryName(executable),
                ref startup, out information), "CreateProcess(CREATE_NEW_CONSOLE)");
            try {
                Process = Process.GetProcessById(information.ProcessId);
                // Cache a handle while the child is alive so ExitCode stays readable after exit.
                GC.KeepAlive(Process.Handle);
            } finally {
                CloseHandle(information.Process);
                CloseHandle(information.Thread);
            }
            Input = new StreamWriter(new FileStream(new SafeFileHandle(inputWrite, true), FileAccess.Write),
                new UTF8Encoding(false)) { AutoFlush = true };
            inputWrite = IntPtr.Zero;
            Output = new StreamReader(new FileStream(new SafeFileHandle(outputRead, true), FileAccess.Read),
                Encoding.UTF8);
            outputRead = IntPtr.Zero;
        } finally {
            if (inputRead != IntPtr.Zero) CloseHandle(inputRead);
            if (inputWrite != IntPtr.Zero) CloseHandle(inputWrite);
            if (outputRead != IntPtr.Zero) CloseHandle(outputRead);
            if (outputWrite != IntPtr.Zero) CloseHandle(outputWrite);
        }
    }

    public void SendCtrlC() {
        // Only this helper joins the child's new console; the JVM never receives the event.
        FreeConsole();
        Require(AttachConsole(Process.Id), "AttachConsole");
        try {
            Require(SetConsoleCtrlHandler(IntPtr.Zero, true), "SetConsoleCtrlHandler");
            Require(GenerateConsoleCtrlEvent(0, 0), "GenerateConsoleCtrlEvent(CTRL_C_EVENT)");
        } finally {
            Require(FreeConsole(), "FreeConsole");
        }
    }

    public void Dispose() {
        Input?.Dispose();
        Output?.Dispose();
        Process?.Dispose();
    }
}
"@

$native = [DotChatNativeConsole]::new((Resolve-Path -LiteralPath $Executable).Path)
try {
    $output = $native.Output.ReadToEndAsync()
    $native.Input.WriteLine("connect $Endpoint")
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (-not (Test-Path -LiteralPath $SignalPath)) {
        if ($native.Process.HasExited) { throw "Native client exited before the Ctrl+C request." }
        if ([DateTime]::UtcNow -ge $deadline) { throw "Timed out waiting for peer-confirmed Ctrl+C request." }
        Start-Sleep -Milliseconds 20
    }
    $native.Process.Refresh()
    $peak = $native.Process.PeakWorkingSet64
    $native.SendCtrlC()
    if (-not $native.Process.WaitForExit(10000)) { throw "Native client did not exit within 10 seconds of Ctrl+C." }
    if (-not $output.Wait(5000)) { throw "Native output pipe did not close." }
    [ordered]@{
        eventDelivered = $true
        exitCode = $native.Process.ExitCode
        output = $output.Result
        peakWorkingSetBytes = $peak
    } | ConvertTo-Json | Set-Content -LiteralPath $OutputPath
} finally {
    if (-not $native.Process.HasExited) { Stop-Process -Id $native.Process.Id -Force }
    $native.Dispose()
}
