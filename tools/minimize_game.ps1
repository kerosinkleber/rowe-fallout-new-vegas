# Minimises the game window for a few seconds and restores it (what alt-tabbing away does), to make
# the game reset its device: powershell -File tools\minimize_game.ps1 [-Seconds 3]
param([int]$Seconds = 3)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class FnvMin {
    [DllImport("user32.dll", CharSet = CharSet.Ansi)] public static extern IntPtr FindWindowA(string cls, string title);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
$h = [FnvMin]::FindWindowA("Fallout: New Vegas", "Fallout: New Vegas")
if ($h -eq [IntPtr]::Zero) { throw "game window not found" }
[void][FnvMin]::ShowWindow($h, 6); Start-Sleep -Seconds $Seconds   # SW_MINIMIZE
[void][FnvMin]::ShowWindow($h, 9); [void][FnvMin]::SetForegroundWindow($h); Start-Sleep -Seconds 2  # SW_RESTORE
"minimised for $Seconds s and restored"
