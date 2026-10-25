# Starts Fallout: New Vegas through xNVSE at a chosen window size and loads the newest manual save.
#
#   powershell -ExecutionPolicy Bypass -File tools\play.ps1 [-Width 1350] [-Height 1080] [-NoLoad]
#
# The size is passed to the mod as rowe.ini [dev] size=WxH, which applies to this one start
# (FalloutPrefs.ini is not changed). The save is loaded through the mod's command file
# (rowe_cmd.txt): it waits for the main menu, then runs the console command `load "<save>"`.
# Manual saves are the .fos files in Documents\My Games\FalloutNV\Saves that are not autosaves
# or quicksaves.
param(
    [int]$Width = 1350,
    [int]$Height = 1080,
    [switch]$NoLoad,
    [string]$Game = ""
)
if (-not $Game) { $Game = (Get-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\Bethesda Softworks\FalloutNV" -ErrorAction SilentlyContinue)."Installed Path" }
if (-not $Game) { throw "Fallout: New Vegas not found in the registry: pass -Game <game folder>" }
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms

if (Get-Process FalloutNV -ErrorAction SilentlyContinue) {
    [System.Windows.Forms.MessageBox]::Show("Fallout: New Vegas is already running.", "ROWE") | Out-Null
    exit 1
}
$plugins = Join-Path $Game "Data\NVSE\Plugins"
# A DLL swapped while the game was running leaves the old one behind (xNVSE ignores it).
$old = Join-Path $plugins "rowe.dll.old"
if (Test-Path $old) { Remove-Item $old }

Add-Type @"
using System.Runtime.InteropServices;
public static class FnvIni {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern bool WritePrivateProfileString(string section, string key, string value, string file);
}
"@
[void][FnvIni]::WritePrivateProfileString("dev", "size", "${Width}x${Height}", (Join-Path $plugins "rowe.ini"))

$commands = @()
if (-not $NoLoad) {
    $saves = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "My Games\FalloutNV\Saves"
    $save = Get-ChildItem $saves -Filter *.fos -ErrorAction SilentlyContinue |
        Where-Object { $_.BaseName -notmatch '^(autosave|quicksave)' } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($save) {
        $commands += "log play.ps1: loading the newest manual save"
        $commands += "waitmenu 1013 3600"
        $commands += "wait 120"
        $commands += "con load `"$($save.BaseName)`""
    }
}
$cmdFile = Join-Path $plugins "rowe_cmd.txt"
if ($commands) { Set-Content -Path $cmdFile -Value $commands -Encoding ASCII }
elseif (Test-Path $cmdFile) { Remove-Item $cmdFile }

Start-Process -FilePath (Join-Path $Game "nvse_loader.exe") -WorkingDirectory $Game
