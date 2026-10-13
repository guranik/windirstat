param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference = 'Stop'
$exePath = (Resolve-Path -LiteralPath $Exe).Path
$fixtures = Join-Path $PSScriptRoot 'fixtures'
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class DiskDiffSmoke {
    public delegate bool EnumProc(IntPtr window, IntPtr param);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr param);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr param);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder text, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowText(IntPtr window, string text);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr menu, int index);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr menu, uint item, uint flags);
    public static string Text(IntPtr window) { var b=new StringBuilder(4096); GetWindowText(window,b,b.Capacity); return b.ToString(); }
    public static string WindowsFor(uint pid) {
        var windows=new List<string>();
        EnumWindows((w,p) => { uint owner; GetWindowThreadProcessId(w,out owner);
            if(owner==pid) { var b=new StringBuilder(256); GetClassName(w,b,b.Capacity);
                windows.Add(String.Format("class={0}, title={1}, visible={2}, snapshot inputs={3}/{4}",
                    b.ToString(), Text(w), IsWindowVisible(w),
                    GetDlgItem(w,1176)!=IntPtr.Zero, GetDlgItem(w,1177)!=IntPtr.Zero)); }
            return true;
        },IntPtr.Zero);
        return windows.Count==0 ? "(none)" : String.Join("; ",windows.ToArray());
    }
    public static IntPtr CompareDialog(uint pid) {
        IntPtr found=IntPtr.Zero;
        EnumWindows((w,p) => { uint owner; GetWindowThreadProcessId(w,out owner);
            if(owner==pid && GetDlgItem(w,1176)!=IntPtr.Zero && GetDlgItem(w,1177)!=IntPtr.Zero) {found=w; return false;} return true;
        },IntPtr.Zero); return found;
    }
    public static IntPtr ComparedFrame(uint pid) {
        IntPtr found=IntPtr.Zero;
        EnumWindows((w,p) => { uint owner; GetWindowThreadProcessId(w,out owner);
            if(owner==pid && Text(w).Contains("Disk Diff |")) {found=w; return false;} return true;
        },IntPtr.Zero); return found;
    }
    public static int LargestListCount(IntPtr parent) {
        int count=0;
        EnumChildWindows(parent,(w,p) => { var b=new StringBuilder(256); GetClassName(w,b,b.Capacity);
            if(b.ToString()=="SysListView32" && IsWindowVisible(w)) count=Math.Max(count,SendMessage(w,0x1004,IntPtr.Zero,IntPtr.Zero).ToInt32()); return true;
        },IntPtr.Zero); return count;
    }
}
'@
function Wait-ForWindow([scriptblock]$GetWindow, [System.Diagnostics.Process]$Process, [string]$Stage) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $window = & $GetWindow
        if ($window -ne [IntPtr]::Zero) { return $window }
        if ($Process.HasExited) { throw "WinDirStat exited before $Stage appeared (exit $($Process.ExitCode))." }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Expected $Stage did not appear within 30 seconds. Process windows: $([DiskDiffSmoke]::WindowsFor([uint32]$Process.Id))"
}
$tempDir = Join-Path ([IO.Path]::GetTempPath()) ('DiskDiffUi-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempDir | Out-Null
$tempExe = Join-Path $tempDir 'WinDirStat-Diff.exe'
Copy-Item -LiteralPath $exePath -Destination $tempExe
New-Item -ItemType File -Path (Join-Path $tempDir 'WinDirStat-Diff.ini') | Out-Null
$process = $null
try {
    $process = Start-Process -FilePath $tempExe -WorkingDirectory $tempDir -PassThru
    Write-Host "Waiting for comparison dialog (PID $($process.Id))."
    $dialog = Wait-ForWindow { [DiskDiffSmoke]::CompareDialog([uint32]$process.Id) } $process 'comparison dialog'
    Write-Host 'Comparison dialog detected.'
    $before = [DiskDiffSmoke]::GetDlgItem($dialog,1176)
    $after = [DiskDiffSmoke]::GetDlgItem($dialog,1177)
    if (-not [DiskDiffSmoke]::SetWindowText($before,(Join-Path $fixtures 'before-en.json'))) { throw 'Cannot fill snapshot A.' }
    if (-not [DiskDiffSmoke]::SetWindowText($after,(Join-Path $fixtures 'after-ru.json'))) { throw 'Cannot fill snapshot B.' }
    [void][DiskDiffSmoke]::SendMessage([DiskDiffSmoke]::GetDlgItem($dialog,1),0x00F5,[IntPtr]::Zero,[IntPtr]::Zero)
    Write-Host 'Waiting for compared main window.'
    $frame = Wait-ForWindow { [DiskDiffSmoke]::ComparedFrame([uint32]$process.Id) } $process 'compared main window'
    if ([DiskDiffSmoke]::Text($frame) -notmatch 'Δ -500') { throw 'Window title does not show the expected -500-byte net change.' }
    if ([DiskDiffSmoke]::LargestListCount($frame) -lt 1) { throw 'Changed-file tree was not populated.' }
    $fileMenu = [DiskDiffSmoke]::GetSubMenu([DiskDiffSmoke]::GetMenu($frame),0)
    if ($fileMenu -eq [IntPtr]::Zero) { throw 'File menu was not found.' }
    [void][DiskDiffSmoke]::SendMessage($frame,0x0117,$fileMenu,[IntPtr]::Zero)
    $resourceText = Get-Content (Join-Path $PSScriptRoot '..\..\windirstat\resource.h') -Raw
    $saveId = [uint32]([regex]::Match($resourceText,'(?m)^#define\s+ID_SAVE_RESULTS\s+(\d+)').Groups[1].Value)
    $state = [DiskDiffSmoke]::GetMenuState($fileMenu,$saveId,0)
    if ($state -eq [uint32]::MaxValue -or ($state -band 3) -eq 0) { throw 'Saving delta data as a normal scan must be disabled.' }
    Write-Host 'Startup comparison dialog, EN/RU loading, delta total, file tree and export guard: passed.'
} finally {
    if ($process -and -not $process.HasExited) {
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(5000)) { $process.Kill(); [void]$process.WaitForExit(5000) }
    }
    Remove-Item -LiteralPath $tempDir -Recurse -Force
}
