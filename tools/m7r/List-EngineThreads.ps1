# Thread inventory of a running engine (M7R R5b.2): starts the engine, waits, then lists
# every thread with its description (SetThreadDescription; the task system names its
# threads) and its Win32 start address as module+offset. Threads started through the C
# runtime (std::thread/std::jthread/std::async) start in ucrtbase; the summary groups
# threads by start module and description. Harness tooling only.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/m7r/List-EngineThreads.ps1 `
#       -Exe out/build/x64-release/bin/IridiumEngine.exe -EngineArgs '--hidden-window --frame-limit 600' -DelaySeconds 8
param(
    [Parameter(Mandatory)] [string] $Exe,
    [string] $EngineArgs = '--hidden-window --frame-limit 100000',
    [double] $DelaySeconds = 8,
    [string] $OutFile = ''
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class IridiumThreadInfo {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr OpenThread(uint access, bool inherit, uint threadId);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")]
    static extern int GetThreadDescription(IntPtr thread, out IntPtr description);
    [DllImport("kernel32.dll")]
    static extern IntPtr LocalFree(IntPtr memory);
    [DllImport("ntdll.dll")]
    static extern int NtQueryInformationThread(IntPtr thread, int infoClass,
        out IntPtr info, int length, IntPtr returned);
    const uint ThreadQueryLimitedInformation = 0x0800;
    const uint ThreadQueryInformation = 0x0040;
    public static string Description(uint threadId) {
        IntPtr thread = OpenThread(ThreadQueryLimitedInformation, false, threadId);
        if (thread == IntPtr.Zero) return "";
        try {
            IntPtr text;
            if (GetThreadDescription(thread, out text) < 0) return "";
            string value = Marshal.PtrToStringUni(text);
            LocalFree(text);
            return value ?? "";
        } finally { CloseHandle(thread); }
    }
    public static long StartAddress(uint threadId) {
        IntPtr thread = OpenThread(ThreadQueryInformation, false, threadId);
        if (thread == IntPtr.Zero) return 0;
        try {
            IntPtr address;
            // ThreadQuerySetWin32StartAddress = 9
            if (NtQueryInformationThread(thread, 9, out address, IntPtr.Size, IntPtr.Zero) != 0) return 0;
            return address.ToInt64();
        } finally { CloseHandle(thread); }
    }
}
'@

$exePath = (Resolve-Path $Exe).Path
$argList = @($EngineArgs -split ' ' | Where-Object { $_ })
$process = Start-Process -FilePath $exePath -ArgumentList $argList -WorkingDirectory (Split-Path $exePath) -PassThru -WindowStyle Hidden
try {
    Start-Sleep -Milliseconds ([int]($DelaySeconds * 1000))
    $process.Refresh()
    if ($process.HasExited) { throw "Engine exited before the snapshot (exit $($process.ExitCode))." }
    $modules = @($process.Modules | ForEach-Object {
        [pscustomobject]@{ name = $_.ModuleName; base = $_.BaseAddress.ToInt64(); size = $_.ModuleMemorySize }
    })
    $rows = foreach ($thread in $process.Threads) {
        $id = [uint32]$thread.Id
        $start = [IridiumThreadInfo]::StartAddress($id)
        $module = $modules | Where-Object { $start -ge $_.base -and $start -lt ($_.base + $_.size) } | Select-Object -First 1
        $where = if ($module) { '{0}+0x{1:x}' -f $module.name, ($start - $module.base) } else { '0x{0:x}' -f $start }
        [pscustomobject]@{
            id = $id
            description = [IridiumThreadInfo]::Description($id)
            module = if ($module) { $module.name } else { '?' }
            start = $where
        }
    }
}
finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}

$lines = @()
$lines += "threads: $(@($rows).Count)"
$lines += ''
$lines += 'by start module and description:'
foreach ($group in ($rows | Group-Object module, description | Sort-Object Count -Descending)) {
    $lines += ('  {0,3}  {1}' -f $group.Count, $group.Name)
}
$lines += ''
$lines += 'by start address:'
foreach ($group in ($rows | Group-Object start | Sort-Object Name)) {
    $descriptions = @($group.Group | ForEach-Object { $_.description } | Where-Object { $_ } | Sort-Object -Unique)
    $lines += ('  {0,3}  {1}  {2}' -f $group.Count, $group.Name, ($descriptions -join ', '))
}
$lines | ForEach-Object { Write-Host $_ }
if ($OutFile) { $lines | Set-Content -Encoding utf8 $OutFile }
