# Starts Fallout: New Vegas through xNVSE and loads a save straight away, without clicking through
# the main menu. Uses ROWE's command file: the plugin waits until the main menu exists and then
# runs the console command `load "<save>"`.
#
#   continue.ps1                 loads the newest save (autosave and quicksave included)
#   continue.ps1 -Save "Save 1   Courier  Goodsprings  00 06 11"   loads that save (name without .fos)
param(
    [string]$Save = "",
    [string]$Game = ""
)
if (-not $Game) { $Game = (Get-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\Bethesda Softworks\FalloutNV" -ErrorAction SilentlyContinue)."Installed Path" }
if (-not $Game) { throw "Fallout: New Vegas not found in the registry: pass -Game <game folder>" }
$ErrorActionPreference = "Stop"

if (Get-Process FalloutNV -ErrorAction SilentlyContinue) { exit 0 }  # already running

$saves = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "My Games\FalloutNV\Saves"
if (-not $Save) {
    $newest = Get-ChildItem $saves -Filter *.fos -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($newest) { $Save = $newest.BaseName }
}

$plugins = Join-Path $Game "Data\NVSE\Plugins"
$cmd = Join-Path $plugins "rowe_cmd.txt"
if ($Save -and (Test-Path (Join-Path $plugins "rowe.dll"))) {
    # `wait` counts presented frames: the main menu needs a moment before it accepts `load`.
    Set-Content -Path $cmd -Encoding ASCII -Value @("wait 300", "con load `"$Save`"")
}
Start-Process -FilePath (Join-Path $Game "nvse_loader.exe") -WorkingDirectory $Game
