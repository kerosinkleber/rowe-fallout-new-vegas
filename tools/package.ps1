# Release archive for mod managers (Mod Organizer 2, Vortex): dist\ROWE-<date>.zip with the files
# relative to the game's Data folder (NVSE\Plugins\rowe.dll, rowe_launcher.exe and a short readme), so MO2 sees a
# valid data layout and installs it without asking. No rowe.ini is included: the plugin writes it
# on first start (under MO2 it lands in Overwrite).
#
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-Version 2026-10-24]
param([string]$Version = (Get-Date -Format "yyyy-MM-dd"))
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $repo "out\rowe.dll"
if (-not (Test-Path $dll)) { throw "out\rowe.dll not found: run build.bat first" }
$launcher = Join-Path $repo "out\rowe_launcher.exe"
if (-not (Test-Path $launcher)) { throw "out\rowe_launcher.exe not found: run build.bat first" }
$mcmEsp = Join-Path $repo "out\ROWE - MCM.esp"
if (-not (Test-Path $mcmEsp)) { throw "out\ROWE - MCM.esp not found: run build.bat (needs Python) first" }

$readme = @"
ROWE for Fallout New Vegas $Version - Reinhold Oberst$([char]0xFC)rcks wunderbare Erweiterungen
FOV slider, lens correction, supersampling, a launcher and small extras for Fallout: New Vegas

Requires FalloutNV.exe 1.4.0.525 and xNVSE 6.x. Start the game through xNVSE (nvse_loader.exe,
or in Mod Organizer 2 the "NVSE" executable).

In game, F8 opens the settings menu (mouse or keyboard; hover a setting for an explanation).
Settings are saved to Data\NVSE\Plugins\rowe.ini; under Mod Organizer 2 that file appears in
Overwrite (move it into this mod to keep it with the mod). A log is written to rowe.log next to it.

Launcher: Data\NVSE\Plugins\rowe_launcher.exe picks a save and starts the game straight into it,
with the vanilla launcher's graphics settings and the mod's supersampling. In Mod Organizer 2, add
it under Executables (binary: this mod's NVSE\Plugins\rowe_launcher.exe) and start it from MO2,
so it sees the profile's saves and INI files and the game it starts runs inside MO2.

The Mod Configuration Menu (optional): with MCM installed, enable "ROWE - MCM.esp" and the
settings also appear in the pause menu under Mod Configuration. The plugin only holds the entry MCM
lists (no scripts); ROWE works the same without it.

ReShade: with supersampling, ReShade's effects are only in the mod's full-size screenshots when
ReShade is the "ReShade with full add-on support" build (reshade.me); with the standard build they
show on screen but the full-size screenshots are taken without them.

No game files are changed. To uninstall, remove the mod (and rowe.ini / rowe.log).
"@

New-Item -ItemType Directory -Force (Join-Path $repo "dist") | Out-Null
$zip = Join-Path $repo "dist\ROWE-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
# Entries are named with forward slashes (Compress-Archive in PowerShell 5.1 writes backslashes,
# which some extractors read as part of the file name).
$archive = [System.IO.Compression.ZipFile]::Open($zip, "Create")
try {
    [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $dll, "NVSE/Plugins/rowe.dll", "Optimal")
    [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $launcher, "NVSE/Plugins/rowe_launcher.exe", "Optimal")
    [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $mcmEsp, "ROWE - MCM.esp", "Optimal")
    # The licences of the code and font built into the DLL (they ask to travel with it).
    $lic = "ROWE (rowe.dll, rowe_launcher.exe) includes the following third-party components.`r`n"
    foreach ($part in @(@("Dear ImGui v1.92.9b (settings menu, launcher)", "third_party\imgui\LICENSE.txt"),
                        @("Share Tech Mono font (settings menu, launcher)", "third_party\fonts\OFL.txt"),
                        @("ReShade add-on API headers v6.6.2", "third_party\reshade\LICENSE.md"))) {
        $lic += "`r`n==== " + $part[0] + " ====`r`n`r`n" + ((Get-Content (Join-Path $repo $part[1]) -Raw) -replace "(?<!`r)`n", "`r`n")
    }
    $le = $archive.CreateEntry("NVSE/Plugins/rowe_licenses.txt")
    $lw = New-Object System.IO.StreamWriter($le.Open(), (New-Object System.Text.UTF8Encoding($false)))
    $lw.Write($lic)
    $lw.Close()
    $entry = $archive.CreateEntry("NVSE/Plugins/rowe_readme.txt")
    $w = New-Object System.IO.StreamWriter($entry.Open(), (New-Object System.Text.UTF8Encoding($false)))
    $w.Write($readme.Replace("`r`n", "`n").Replace("`n", "`r`n"))
    $w.Close()
} finally { $archive.Dispose() }
"wrote $zip"
$check = [System.IO.Compression.ZipFile]::OpenRead($zip)
try { $check.Entries | ForEach-Object { "  $($_.FullName)  $($_.Length) bytes" } } finally { $check.Dispose() }
