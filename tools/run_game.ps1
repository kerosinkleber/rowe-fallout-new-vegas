# Starts one test run: closes a running game, archives the previous run's plugin output to
# runs\<previous name> (recorded in runs\last.txt), installs out\rowe.dll and the test command file, and starts the game through
# xNVSE. Does not wait; watch rowe_state.txt / rowe.log in the Plugins folder.
#
#   powershell -File tools\run_game.ps1 -Name run4 -Test tools\tests\fov_recon.txt
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Test = "",
    [string]$Game = ""
)
if (-not $Game) { $Game = (Get-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\Bethesda Softworks\FalloutNV" -ErrorAction SilentlyContinue)."Installed Path" }
if (-not $Game) { throw "Fallout: New Vegas not found in the registry: pass -Game <game folder>" }
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$plugins = Join-Path $Game "Data\NVSE\Plugins"

Get-Process FalloutNV -ErrorAction SilentlyContinue | Stop-Process -Force
for ($i = 0; $i -lt 20 -and (Get-Process FalloutNV -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 250 }

$runs = Join-Path $repo "runs"
if (Test-Path (Join-Path $runs $Name)) { throw "runs\$Name already exists" }
$lastFile = Join-Path $runs "last.txt"
$previous = if (Test-Path $lastFile) { (Get-Content $lastFile -TotalCount 1).Trim() } else { "unnamed_" + (Get-Date -Format yyyyMMdd_HHmmss) }
$archive = Join-Path $runs $previous
New-Item -ItemType Directory -Force $archive | Out-Null
foreach ($f in "rowe.log", "rowe_state.txt", "rowe_trace.txt", "rowe_fovtrace.csv") {
    $p = Join-Path $plugins $f
    if (Test-Path $p) { Move-Item -Force $p (Join-Path $archive $f) }
}
# Screenshots and their state snapshots: rowe_<date>_<time>*.png / .txt
Get-ChildItem $plugins -Filter "rowe_2*" | Move-Item -Destination $archive -Force
Set-Content -Path $lastFile -Value $Name
foreach ($f in "rowe_state.tmp", "rowe_trace.req", "rowe_cmd.txt", "rowe.dll.old") {
    $p = Join-Path $plugins $f
    if (Test-Path $p) { Remove-Item $p }
}
Copy-Item (Join-Path $repo "out\rowe.dll") $plugins
if ($Test) { Copy-Item (Join-Path $repo $Test) (Join-Path $plugins "rowe_cmd.txt") }
Start-Process -FilePath (Join-Path $Game "nvse_loader.exe") -WorkingDirectory $Game
"started $Name (test: $Test)"
