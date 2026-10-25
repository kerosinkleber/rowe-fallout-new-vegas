# Captures the game window's client area from the screen (what is really shown, including overlays
# such as ReShade's, which the mod's own screenshots do not contain), optionally after tapping a key
# with real keyboard input (scan code, hex; 0x100 = extended, e.g. 147 = Home):
#   powershell -File tools\screen_capture.ps1 -Out shot.png [-Key 147]
param([Parameter(Mandatory = $true)][string]$Out, [string]$Key = "", [string]$Class = "Fallout: New Vegas")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class FnvCap {
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] i, int size);
    [DllImport("user32.dll", CharSet = CharSet.Ansi)] public static extern IntPtr FindWindowA(string cls, string title);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public static void Key(ushort scan, bool up) {
        INPUT[] i = new INPUT[1]; i[0].type = 1; i[0].ki.wScan = (ushort)(scan & 0xFF);
        i[0].ki.dwFlags = 8u | (up ? 2u : 0u) | ((scan & 0x100) != 0 ? 1u : 0u);
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@
[void][FnvCap]::SetProcessDPIAware()
$h = [FnvCap]::FindWindowA($Class, [NullString]::Value)  # -Class rowe_launcher: the launcher
if ($h -eq [IntPtr]::Zero) { throw "game window not found" }
if ($Key) {
    $code = [Convert]::ToUInt16($Key, 16)
    [FnvCap]::Key($code, $false); Start-Sleep -Milliseconds 80; [FnvCap]::Key($code, $true); Start-Sleep -Milliseconds 800
}
$r = New-Object FnvCap+RECT; [void][FnvCap]::GetClientRect($h, [ref]$r)
$p = New-Object FnvCap+POINT; [void][FnvCap]::ClientToScreen($h, [ref]$p)
$bmp = New-Object System.Drawing.Bitmap ($r.r - $r.l), ($r.b - $r.t)
[System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($p.x, $p.y, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"saved $Out ($($bmp.Width)x$($bmp.Height))"
