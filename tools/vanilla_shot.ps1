# Screenshot of the game WITHOUT the mod: moves rowe.dll out of the plugin folder, starts the
# game through xNVSE (FalloutPrefs.ini decides the size and window mode), loads a save and runs
# console lines by typing them (SendInput scan codes; the console reads US positions), optionally
# holds the right mouse button (aim), captures the game window from the screen, quits, and puts
# rowe.dll back (also on failure). The game window must stay in front while it runs.
#
#   powershell -ExecutionPolicy Bypass -File tools\vanilla_shot.ps1 -Save "ROWE sniper rifle" `
#       -Commands "player.equipitem 00004353" -Aim -Out screenshots\vanilla.png [-Keys 2F]
# -Drag <dx>: saves a first capture as <Out>_before.png, then holds the left button, moves the
# mouse dx counts right (after -Pre dx,dy counts without the button) and saves <Out> after it.
param(
    [Parameter(Mandatory = $true)][string]$Save,
    [string[]]$Commands = @(),
    [string[]]$Keys = @(),  # scan codes (hex) tapped after the commands, e.g. 2F = V (VATS)
    [switch]$Aim,
    [int]$Drag = 0,
    [int[]]$Pre = @(),
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Game = "",
    [int]$MenuSeconds = 25,
    [int]$LoadSeconds = 20
)
if (-not $Game) { $Game = (Get-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\Bethesda Softworks\FalloutNV" -ErrorAction SilentlyContinue)."Installed Path" }
if (-not $Game) { throw "Fallout: New Vegas not found in the registry: pass -Game <game folder>" }
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$plugin = Join-Path $Game "Data\NVSE\Plugins\rowe.dll"
$parked = Join-Path $repo "backups\vanilla_shot_rowe.dll"
if (Get-Process FalloutNV -ErrorAction SilentlyContinue) { throw "the game is running; close it first" }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class FnvInput {
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint data, dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit)] public struct INPUT {
        [FieldOffset(0)] public uint type;
        [FieldOffset(8)] public KEYBDINPUT ki;
        [FieldOffset(8)] public MOUSEINPUT mi;
    }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll", CharSet = CharSet.Ansi)] public static extern IntPtr FindWindowA(string cls, string title);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public static void Key(ushort scan, bool up) {
        INPUT[] i = new INPUT[1];
        i[0].type = 1;
        i[0].ki.wScan = (ushort)(scan & 0xFF);
        i[0].ki.dwFlags = 8u | (up ? 2u : 0u) | ((scan & 0x100) != 0 ? 1u : 0u);  // SCANCODE, KEYUP, EXTENDED
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Mouse(int dx, int dy, uint flags) {
        INPUT[] i = new INPUT[1];
        i[0].type = 0; i[0].mi.dx = dx; i[0].mi.dy = dy; i[0].mi.dwFlags = flags;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void RightButton(bool up) {
        INPUT[] i = new INPUT[1];
        i[0].type = 0;
        i[0].mi.dwFlags = up ? 0x10u : 0x08u;  // RIGHTUP / RIGHTDOWN
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@
[void][FnvInput]::SetProcessDPIAware()

# US scan codes for console text: [char] -> code (0x100 = with shift).
$codes = @{}
$rows = @{ 0x10 = "qwertyuiop"; 0x1E = "asdfghjkl"; 0x2C = "zxcvbnm" }
foreach ($start in $rows.Keys) { $s = $rows[$start]; for ($k = 0; $k -lt $s.Length; $k++) { $codes[[string]$s[$k]] = $start + $k } }
$digits = "1234567890"; for ($k = 0; $k -lt 10; $k++) { $codes[[string]$digits[$k]] = 0x02 + $k }
$codes[" "] = 0x39; $codes["."] = 0x34; $codes[","] = 0x33; $codes["-"] = 0x0C; $codes['"'] = 0x128

function Tap([int]$scan, [int]$ms = 50) {
    $shift = ($scan -band 0x100) -ne 0
    if ($shift) { [FnvInput]::Key(0x2A, $false); Start-Sleep -Milliseconds 20 }
    [FnvInput]::Key($scan -band 0xFF, $false); Start-Sleep -Milliseconds $ms
    [FnvInput]::Key($scan -band 0xFF, $true); Start-Sleep -Milliseconds 30
    if ($shift) { [FnvInput]::Key(0x2A, $true); Start-Sleep -Milliseconds 20 }
}
function Console([string]$line, [switch]$StayOpen) {
    Tap 0x29 80; Start-Sleep -Milliseconds 400         # open the console (key left of 1)
    foreach ($c in $line.ToLower().ToCharArray()) {
        $code = $codes[[string]$c]
        if ($null -eq $code) { throw "no scan code for '$c'" }
        Tap $code
    }
    Tap 0x1C 60; Start-Sleep -Milliseconds 400         # Enter
    if (-not $StayOpen) { Tap 0x29 80; Start-Sleep -Milliseconds 300 }  # close the console
    "console: $line"
}
function Front($h) {
    [void][FnvInput]::ShowWindow($h, 6); Start-Sleep -Milliseconds 300
    [void][FnvInput]::ShowWindow($h, 9); [void][FnvInput]::SetForegroundWindow($h); Start-Sleep -Milliseconds 500
    if ([FnvInput]::GetForegroundWindow() -ne $h) { throw "could not bring the game window to the front" }
}

New-Item -ItemType Directory -Force (Split-Path -Parent $parked) | Out-Null
Copy-Item $plugin $parked -Force
Remove-Item $plugin
"rowe.dll parked at $parked"
try {
    Start-Process -FilePath (Join-Path $Game "nvse_loader.exe") -WorkingDirectory $Game
    $h = [IntPtr]::Zero
    for ($k = 0; $k -lt 120 -and $h -eq [IntPtr]::Zero; $k++) { Start-Sleep -Milliseconds 500; $h = [FnvInput]::FindWindowA("Fallout: New Vegas", "Fallout: New Vegas") }
    if ($h -eq [IntPtr]::Zero) { throw "game window not found" }
    Start-Sleep -Seconds $MenuSeconds
    Front $h
    # The console stays open through the load (a key pressed while loading is lost): close it after.
    # (Bringing the window to the front again would open the pause menu.)
    Console "load `"$Save`"" -StayOpen
    Start-Sleep -Seconds $LoadSeconds
    Tap 0x29 80; Start-Sleep -Milliseconds 500
    foreach ($line in $Commands) { Console $line; Start-Sleep -Seconds 3 }
    foreach ($k in $Keys) { Tap ([Convert]::ToInt32($k, 16)) 80; Start-Sleep -Milliseconds 2500 }
    if ($Aim) { [FnvInput]::RightButton($false); Start-Sleep -Milliseconds 2500 }
    $outPath = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $repo $Out }
    New-Item -ItemType Directory -Force (Split-Path -Parent $outPath) | Out-Null
    function Capture([string]$path) {
        $r = New-Object FnvInput+RECT; [void][FnvInput]::GetClientRect($h, [ref]$r)
        $p = New-Object FnvInput+POINT; [void][FnvInput]::ClientToScreen($h, [ref]$p)
        $bmp = New-Object System.Drawing.Bitmap ($r.r - $r.l), ($r.b - $r.t)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($p.x, $p.y, 0, 0, $bmp.Size)
        $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
        "saved $path ($($bmp.Width)x$($bmp.Height))"
    }
    if ($Drag -ne 0) {
        if ($Pre.Count -eq 2) { for ($k = 0; $k -lt 10; $k++) { [FnvInput]::Mouse([int]($Pre[0] / 10), [int]($Pre[1] / 10), 1); Start-Sleep -Milliseconds 30 } }
        Start-Sleep -Milliseconds 800
        Capture ($outPath -replace '\.png$', '_before.png')
        [FnvInput]::Mouse(0, 0, 2); Start-Sleep -Milliseconds 300
        for ($k = 0; $k -lt 20; $k++) { [FnvInput]::Mouse([int]($Drag / 20), 0, 1); Start-Sleep -Milliseconds 30 }
        Start-Sleep -Milliseconds 300; [FnvInput]::Mouse(0, 0, 4); Start-Sleep -Milliseconds 800
    }
    Capture $outPath
    if ($Aim) { [FnvInput]::RightButton($true); Start-Sleep -Milliseconds 300 }
    Console "qqq"
    for ($k = 0; $k -lt 40 -and (Get-Process FalloutNV -ErrorAction SilentlyContinue); $k++) { Start-Sleep -Milliseconds 500 }
} finally {
    if (Get-Process FalloutNV -ErrorAction SilentlyContinue) { Get-Process FalloutNV | Stop-Process -Force; Start-Sleep -Seconds 2 }
    Copy-Item $parked $plugin -Force
    "rowe.dll restored"
}
