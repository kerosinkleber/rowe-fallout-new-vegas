#!/bin/sh
# Frame rate and video memory around VATS: runs tools/tests/$TEST (default vats_trace.txt) with the given
# supersampling while nvidia-smi samples memory once a second, then prints, per phase (before
# VATS, in VATS, after), the frames per second from the state file and the peak memory.
#   sh tools/vats_perf.sh <run name> <supersample> [WxH]
# Uses the helper that writes rowe.ini from backups (see TESTING.md); needs nvidia-smi.
cd "$(dirname "$0")/.." || exit 1
GAME="${FNV_GAME:-$(reg query 'HKLM\SOFTWARE\WOW6432Node\Bethesda Softworks\FalloutNV' //v 'Installed Path' 2>/dev/null | sed -n 's/.*REG_SZ *//p' | tr -d '\r')}"  # or set FNV_GAME
P="$(cygpath -u "$GAME")/Data/NVSE/Plugins"
OUT="runs/$1.gpu.csv"
mkdir -p runs
( while :; do echo "$(date +%H:%M:%S),$(nvidia-smi --query-gpu=memory.used,utilization.gpu --format=csv,noheader,nounits)"; sleep 1; done ) > "$OUT" &
SAMPLER=$!
SS=$2 sh "${EXP:?set EXP to the run helper}" "$1" 0 tools/tests/${TEST:-vats_trace.txt} "${3:-1300x1300}" > /dev/null
kill $SAMPLER
python - "$P/rowe.log" "$OUT" <<'EOF'
import sys, re, datetime
log = open(sys.argv[1], encoding="latin-1").read().splitlines()
marks = {}
for line in log:
    m = re.match(r"(\d\d:\d\d:\d\d)\.\d+ .*cmd: ---- (normal frame|find a vats target|enter vats|vats frame|after vats) ----", line)
    if m: marks[m.group(2)] = m.group(1)
gpu = [l.split(",") for l in open(sys.argv[2]) if l.count(",") >= 2]
def t(s): return datetime.datetime.strptime(s, "%H:%M:%S")
def peak(a, b):
    v = [int(m) for ts, m, u in gpu if t(a) <= t(ts) <= t(b)]
    return max(v) if v else None
print("marks", marks)
def fps(a, b, frames):
    if a in marks and b in marks:
        s = (t(marks[b]) - t(marks[a])).total_seconds()
        return round(frames / s, 1) if s > 0 else None
# Frames between the marks (the test's waits count presented frames).
print("fps before VATS:", fps("normal frame", "find a vats target", 91), " fps in VATS:", fps("enter vats", "vats frame", 300))
if "enter vats" in marks:
    print("memory before VATS (MiB):", peak(marks.get("normal frame", marks["enter vats"]), marks.get("find a vats target", marks["enter vats"])))
    print("memory in VATS (MiB):    ", peak(marks["enter vats"], marks.get("after vats", marks["enter vats"])))
EOF
