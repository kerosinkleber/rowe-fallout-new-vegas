# Real mouse input on the game window (client coordinates): moves the cursor to X,Y and clicks, or
# with -ToX/-ToY drags from X,Y to there with the left button held.
#   powershell -File tools\mouse_click.ps1 -X 175 -Y 119 [-ToX 700 -ToY 160]
param([int]$X, [int]$Y, [int]$ToX = -1, [int]$ToY = -1, [string]$Class = "Fallout: New Vegas")
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class FnvClick {
    [StructLayout(LayoutKind.Sequential)] public struct P { public int x, y; }
    [StructLayout(LayoutKind.Sequential)] public struct MI { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit)] public struct IN { [FieldOffset(0)] public uint type; [FieldOffset(8)] public MI mi; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, IN[] i, int s);
    public static void Button(uint f) { IN[] i = new IN[1]; i[0].type = 0; i[0].mi.flags = f; SendInput(1, i, Marshal.SizeOf(typeof(IN))); }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll", CharSet = CharSet.Ansi)] public static extern IntPtr FindWindowA(string c, string t);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref P p);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[void][FnvClick]::SetProcessDPIAware()
$h = [FnvClick]::FindWindowA($Class, [NullString]::Value)  # -Class rowe_launcher: the launcher
function To([int]$cx, [int]$cy) { $p = New-Object FnvClick+P; $p.x = $cx; $p.y = $cy; [void][FnvClick]::ClientToScreen($h, [ref]$p); [void][FnvClick]::SetCursorPos($p.x, $p.y) }
To $X $Y; Start-Sleep -Milliseconds 250
[FnvClick]::Button(2); Start-Sleep -Milliseconds 120   # LEFTDOWN
if ($ToX -ge 0) {
    for ($k = 1; $k -le 20; $k++) { To ([int]($X + ($ToX - $X) * $k / 20)) ([int]($Y + ($ToY - $Y) * $k / 20)); Start-Sleep -Milliseconds 25 }
}
[FnvClick]::Button(4); Start-Sleep -Milliseconds 300   # LEFTUP
"clicked $X,$Y$(if ($ToX -ge 0) { " dragged to $ToX,$ToY" })"
