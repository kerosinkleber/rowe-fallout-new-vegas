#!/bin/sh
# Waits until FalloutNV.exe exits (or a timeout in seconds, default 300), then prints the plugin
# log's command, screenshot, crash and error lines.
#   sh tools/wait_game.sh [timeout]
GAME="${FNV_GAME:-$(reg query 'HKLM\SOFTWARE\WOW6432Node\Bethesda Softworks\FalloutNV' //v 'Installed Path' 2>/dev/null | sed -n 's/.*REG_SZ *//p' | tr -d '\r')}"  # or set FNV_GAME
P="$(cygpath -u "$GAME")/Data/NVSE/Plugins"
timeout=${1:-300}
sleep 5
end=$((SECONDS + timeout))
while [ $SECONDS -lt $end ]; do
    tasklist //FI "IMAGENAME eq FalloutNV.exe" | grep -q FalloutNV.exe || { echo "game exited"; break; }
    sleep 2
done
[ $SECONDS -ge $end ] && echo "TIMEOUT: game still running"
grep -E "cmd: |CRASH|screenshot|FAILED|failed|SKIPPED|NOT FOUND|ExitGame|budget" "$P/rowe.log" | cut -c1-200 | tail -60
