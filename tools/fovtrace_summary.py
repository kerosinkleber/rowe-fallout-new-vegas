"""Condenses rowe_fovtrace.csv into a per-frame timeline.

Usage: python -I tools/fovtrace_summary.py <rowe_fovtrace.csv> [<rowe.log>]

Each output line is one frame where something changed: the FOV each call site asked for, the
player's FOV fields, the third-person bytes and the menu flag. With the log, the `cmd:` markers
are placed at the frame they ran, so the timeline reads alongside the test script.
"""
import csv
import sys
from collections import OrderedDict


def main(path, log_path=None):
    frames = OrderedDict()
    for row in csv.DictReader(open(path)):
        f = int(row["frame"])
        d = frames.setdefault(f, {"sites": OrderedDict(), "row": row})
        if row["kind"] in "SC":
            key = row["kind"] + row["ret"][-6:]
            d["sites"][key] = float(row["fov"])
        d["row"] = row

    markers = {}
    if log_path:
        import re
        # Log lines carry wall-clock times; the trace rows carry ms since trace start. Align them
        # through the "fovtrace: started ... at frame N" line.
        lines = open(log_path, errors="replace").read().splitlines()
        start = None
        for ln in lines:
            m = re.match(r"(\d+):(\d+):(\d+)\.(\d+) \[\d+\] fovtrace: started for \d+ frames at frame (\d+)", ln)
            if m:
                start = (int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3])) * 1000 + int(m[4])
        if start is not None:
            times = [(float(r["row"]["t_ms"]), f) for f, r in frames.items()]
            for ln in lines:
                m = re.match(r"(\d+):(\d+):(\d+)\.(\d+) \[\d+\] cmd: (?:---- (.*) ----|(con .*))", ln)
                if not m:
                    continue
                t = (int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3])) * 1000 + int(m[4]) - start
                if t < 0:
                    continue
                f = next((fr for tt, fr in times if tt >= t), None)
                if f is not None:
                    markers.setdefault(f, []).append(m[5] or m[6])

    last = None
    for f, d in frames.items():
        r = d["row"]
        sites = " ".join(f"{k}={v:g}" for k, v in d["sites"].items())
        state = (sites, r["pl_world"], r["pl_1st"], r["pl_shoulder"], r["b64A"], r["b64C"], r["menumode"])
        for m in markers.get(f, []):
            print(f"{f:6d}   >>> {m}")
        if state != last:
            print(f"{f:6d} pl {r['pl_world']}/{r['pl_1st']}/{r['pl_shoulder']} 3rd {r['b64A']}/{r['b64C']} menu {r['menumode']} | {sites}")
            last = state


if __name__ == "__main__":
    main(*sys.argv[1:3])
