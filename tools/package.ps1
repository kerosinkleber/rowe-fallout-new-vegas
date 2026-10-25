# Release archives:
# - dist\ROWE-<date>.zip for mod managers (Mod Organizer 2, Vortex): a FOMOD installer. The core
#   files (relative to the game's Data folder: NVSE\Plugins\rowe.dll, rowe_launcher.exe, readme,
#   licences) always; options: the MCM page (ROWE - MCM.esp), and for Mod Organizer 2 "start the
#   game with the ROWE launcher" and "desktop shortcut" (marker files the plugin acts on at the first
#   start through MO2: src\mo2setup.cpp, src\launcher\mo2helper.cpp).
# - dist\ROWE-<date>-manual.zip: the plain Data layout (everything, no options) to copy by hand.
# No rowe.ini is included: the plugin writes it on first start (under MO2 it lands in Overwrite).
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
FOV slider, lens correction, supersampling, a launcher, photo mode and small extras for Fallout: New Vegas

Requires FalloutNV.exe 1.4.0.525 and xNVSE 6.x. Start the game through xNVSE (nvse_loader.exe,
or in Mod Organizer 2 the "NVSE" executable), or through the ROWE launcher.

In game, F8 opens the settings menu (mouse or keyboard; hover a setting for an explanation).
Settings are saved to Data\NVSE\Plugins\rowe.ini; under Mod Organizer 2 that file appears in
Overwrite (move it into this mod to keep it with the mod). A log is written to rowe.log next to it.

Photo mode (F10): fly anywhere, invisible and undetectable, the world loading as you go. Space /
Ctrl fly up and down, Shift flies faster, the mouse wheel zooms. F10 again puts you back where you
started; opening a menu ends it too, and Quicksave is off while it is on.

Launcher: Data\NVSE\Plugins\rowe_launcher.exe picks a save and starts the game straight into it,
with the vanilla launcher's graphics settings and the mod's supersampling. Installed with Mod
Organizer 2's installer and "Start the game with the ROWE launcher" ticked, it becomes MO2's start
option by itself: start the game once through MO2 (any executable), close MO2, and from the next
MO2 start "ROWE Launcher" is selected (and the desktop shortcut is there, if ticked). The log of
that step is rowe_mo2setup.log in MO2's logs folder; ModOrganizer.ini is backed up next to itself
first. By hand: add it under Executables (binary: this mod's NVSE\Plugins\rowe_launcher.exe).

The Mod Configuration Menu (optional): with MCM installed, enable "ROWE - MCM.esp" and the
settings also appear in the pause menu under Mod Configuration. The plugin only holds the entry MCM
lists (no scripts); ROWE works the same without it.

ReShade: with supersampling, ReShade's effects are only in the mod's full-size screenshots when
ReShade is the "ReShade with full add-on support" build (reshade.me); with the standard build they
show on screen but the full-size screenshots are taken without them.

No game files are changed. To uninstall, remove the mod (and rowe.ini / rowe.log).
"@

# The licences of the code and font built into the DLL (they ask to travel with it).
$lic = "ROWE (rowe.dll, rowe_launcher.exe) includes the following third-party components.`r`n"
foreach ($part in @(@("Dear ImGui v1.92.9b (settings menu, launcher)", "third_party\imgui\LICENSE.txt"),
                    @("Share Tech Mono font (settings menu, launcher)", "third_party\fonts\OFL.txt"),
                    @("ReShade add-on API headers v6.6.2", "third_party\reshade\LICENSE.md"))) {
    $lic += "`r`n==== " + $part[0] + " ====`r`n`r`n" + ((Get-Content (Join-Path $repo $part[1]) -Raw) -replace "(?<!`r)`n", "`r`n")
}

$infoXml = @"
<?xml version="1.0" encoding="UTF-8"?>
<fomod>
  <Name>ROWE for Fallout New Vegas</Name>
  <Author>kerosinkleber</Author>
  <Version>$Version</Version>
  <Website>https://github.com/kerosinkleber/rowe-fallout-new-vegas</Website>
  <Description>FOV slider, lens correction, supersampling, a launcher, photo mode and small extras (xNVSE plugin).</Description>
  <Groups><element>Visuals and Graphics</element></Groups>
</fomod>
"@

$moduleXml = @'
<?xml version="1.0" encoding="UTF-8"?>
<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://qconsulting.ca/fo3/ModConfig5.0.xsd">
  <moduleName>ROWE for Fallout New Vegas</moduleName>
  <requiredInstallFiles>
    <folder source="core" destination="" />
  </requiredInstallFiles>
  <installSteps order="Explicit">
    <installStep name="Options">
      <optionalFileGroups order="Explicit">
        <group name="The Mod Configuration Menu" type="SelectAny">
          <plugins order="Explicit">
            <plugin name="MCM page">
              <description>ROWE's settings also in the pause menu under Mod Configuration (installs "ROWE - MCM.esp"; needs The Mod Configuration Menu). ROWE works the same without it.</description>
              <files>
                <file source="mcm\ROWE - MCM.esp" destination="ROWE - MCM.esp" />
              </files>
              <typeDescriptor>
                <dependencyType>
                  <defaultType name="Optional" />
                  <patterns>
                    <pattern>
                      <dependencies operator="And">
                        <fileDependency file="The Mod Configuration Menu.esp" state="Active" />
                      </dependencies>
                      <type name="Recommended" />
                    </pattern>
                  </patterns>
                </dependencyType>
              </typeDescriptor>
            </plugin>
          </plugins>
        </group>
        <group name="Mod Organizer 2" type="SelectAny">
          <plugins order="Explicit">
            <plugin name="Start the game with the ROWE launcher">
              <description>Makes the ROWE launcher Mod Organizer 2's start option (adds it under Executables if needed and selects it). It is done by itself after the first game start through MO2, once you close MO2 (MO2 would otherwise overwrite it): from the next MO2 start, "ROWE Launcher" is selected. ModOrganizer.ini is backed up first. Mod Organizer 2 only.</description>
              <files>
                <folder source="setup_start" destination="" />
              </files>
              <typeDescriptor>
                <type name="Recommended" />
              </typeDescriptor>
            </plugin>
            <plugin name="Desktop shortcut">
              <description>A desktop shortcut "Fallout New Vegas (ROWE)" that starts the ROWE launcher through Mod Organizer 2 (created at the same time as the start option above, once you close MO2). Windows will ask for permission when it is created: allow it, or there is no shortcut. Mod Organizer 2 only.</description>
              <files>
                <folder source="setup_shortcut" destination="" />
              </files>
              <typeDescriptor>
                <type name="Optional" />
              </typeDescriptor>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>
'@

New-Item -ItemType Directory -Force (Join-Path $repo "dist") | Out-Null
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
# Entries are named with forward slashes (Compress-Archive in PowerShell 5.1 writes backslashes,
# which some extractors read as part of the file name).
function Add-Text($archive, $name, $text) {
    $entry = $archive.CreateEntry($name)
    $w = New-Object System.IO.StreamWriter($entry.Open(), (New-Object System.Text.UTF8Encoding($false)))
    $w.Write($text.Replace("`r`n", "`n").Replace("`n", "`r`n"))
    $w.Close()
}
function Add-File($archive, $path, $name) {
    [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $path, $name, "Optimal")
}
function Open-Zip($name) {
    $zip = Join-Path $repo "dist\$name"
    if (Test-Path $zip) { Remove-Item $zip }
    return @($zip, [System.IO.Compression.ZipFile]::Open($zip, "Create"))
}

# The installer archive.
$z = Open-Zip "ROWE-$Version.zip"
try {
    Add-Text $z[1] "fomod/info.xml" $infoXml
    Add-Text $z[1] "fomod/ModuleConfig.xml" $moduleXml
    Add-File $z[1] $dll "core/NVSE/Plugins/rowe.dll"
    Add-File $z[1] $launcher "core/NVSE/Plugins/rowe_launcher.exe"
    Add-Text $z[1] "core/NVSE/Plugins/rowe_licenses.txt" $lic
    Add-Text $z[1] "core/NVSE/Plugins/rowe_readme.txt" $readme
    Add-File $z[1] $mcmEsp "mcm/ROWE - MCM.esp"
    Add-Text $z[1] "setup_start/NVSE/Plugins/rowe_setup_start.txt" "Installer option: make the ROWE launcher Mod Organizer 2's start option (done once by the plugin, then this file is removed)."
    Add-Text $z[1] "setup_shortcut/NVSE/Plugins/rowe_setup_shortcut.txt" "Installer option: create a desktop shortcut for the ROWE launcher through Mod Organizer 2 (done once by the plugin, then this file is removed)."
} finally { $z[1].Dispose() }
"wrote $($z[0])"

# The manual archive.
$m = Open-Zip "ROWE-$Version-manual.zip"
try {
    Add-File $m[1] $dll "NVSE/Plugins/rowe.dll"
    Add-File $m[1] $launcher "NVSE/Plugins/rowe_launcher.exe"
    Add-File $m[1] $mcmEsp "ROWE - MCM.esp"
    Add-Text $m[1] "NVSE/Plugins/rowe_licenses.txt" $lic
    Add-Text $m[1] "NVSE/Plugins/rowe_readme.txt" $readme
} finally { $m[1].Dispose() }
"wrote $($m[0])"

foreach ($zip in @($z[0], $m[0])) {
    $check = [System.IO.Compression.ZipFile]::OpenRead($zip)
    try { "${zip}:"; $check.Entries | ForEach-Object { "  $($_.FullName)  $($_.Length) bytes" } } finally { $check.Dispose() }
}
