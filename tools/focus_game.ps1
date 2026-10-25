# Brings the game window to the front (minimise, then restore, then SetForegroundWindow), so that
# injected keys reach the game in an unattended test. Prints whether the game is in front after.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class FnvWin {
    [DllImport("user32.dll", CharSet = CharSet.Ansi)] public static extern IntPtr FindWindowA(string cls, string title);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
"@
$h = [FnvWin]::FindWindowA("Fallout: New Vegas", "Fallout: New Vegas")
if ($h -eq [IntPtr]::Zero) { "game window not found"; exit 1 }
[void][FnvWin]::ShowWindow($h, 6)   # SW_MINIMIZE
Start-Sleep -Milliseconds 300
[void][FnvWin]::ShowWindow($h, 9)   # SW_RESTORE
[void][FnvWin]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 300
"in front: " + ([FnvWin]::GetForegroundWindow() -eq $h)
