# Sends a relative mouse move (SendInput, MOUSEEVENTF_MOVE) in steps, for measuring cursor speed in
# the game: powershell -File tools\mouse_move.ps1 -Dx 200 [-Dy 0] [-Steps 20] [-Hold]
# -Hold presses the left button before the move and releases it after (dragging, e.g. the Pip-Boy map).
param([int]$Dx = 0, [int]$Dy = 0, [int]$Steps = 20, [switch]$Hold)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class FnvMouse {
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public MOUSEINPUT mi; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] i, int size);
    public static void Send(int dx, int dy, uint flags) {
        INPUT[] i = new INPUT[1]; i[0].type = 0; i[0].mi.dx = dx; i[0].mi.dy = dy; i[0].mi.flags = flags;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Move(int dx, int dy) { Send(dx, dy, 1); }
}
"@
if ($Hold) { [FnvMouse]::Send(0, 0, 2); Start-Sleep -Milliseconds 300 }  # LEFTDOWN
for ($k = 0; $k -lt $Steps; $k++) { [FnvMouse]::Move([int]($Dx / $Steps), [int]($Dy / $Steps)); Start-Sleep -Milliseconds 30 }
if ($Hold) { Start-Sleep -Milliseconds 300; [FnvMouse]::Send(0, 0, 4) }  # LEFTUP
"moved $Dx $Dy$(if ($Hold) { ' (dragged)' })"
