#!/usr/bin/env python3
"""Group e2e findings by a stable signature (first in-project frames).
usage: triage.py CX_BINARY FINDINGS_DIR..."""
import glob, json, os, re, subprocess, sys

cx = sys.argv[1]
SRC = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "src") + "/"


def addr2line(offsets):
    if not offsets:
        return []
    out = subprocess.run(["addr2line", "-e", cx, "-f", "-C", "-i"] + offsets, capture_output=True, text=True).stdout
    return out.splitlines()


def frames(det):
    """Return list of 'func @ file:line' for in-project frames."""
    res = []
    ub = det.get("ubsan", "")
    if ub:
        head = next((l for l in ub.splitlines() if "runtime error" in l), "")
        res.append("UB:" + re.sub(r"0x[0-9a-f]+", "ADDR", head.split(SRC)[-1])[:160])
        for l in ub.splitlines():
            m = re.search(r" in (.+?) (" + re.escape(SRC) + r"\S+)", l)
            if m:
                res.append(m.group(1)[:70] + " @ " + m.group(2).replace(SRC, ""))
        return res
    cl = det.get("crashlog", "")
    if cl:
        offs = re.findall(r"/cx\(\+?(0x[0-9a-f]+)\)", cl) or re.findall(r"cx\((?:[^+)]*)\+(0x[0-9a-f]+)\)", cl)
        # absolute addresses printed for the main binary when symbol lookup fails
        lines = addr2line(offs)
        for i in range(0, len(lines) - 1, 2):
            fn, loc = lines[i], lines[i + 1]
            if SRC in loc:
                res.append(fn[:70] + " @ " + loc.replace(SRC, "").split(" ")[0])
        sig = re.search(r"SIGNAL (\d+)", cl)
        res.insert(0, "SIG" + (sig.group(1) if sig else "?"))
    if det.get("cx_errors"):
        res.append("EXC:" + det["cx_errors"].strip().splitlines()[-1].split("] ", 1)[-1][:160])
    if det.get("guardlog"):
        for l in det["guardlog"].splitlines():
            if "sh-replaced" not in l and "exec-refused" not in l:
                res.append("GUARD:" + re.sub(r"/tmp/\S+?/work", "WORK", l)[:120])
                break
    return res


groups = {}
for d in sys.argv[2:]:
    for f in sorted(glob.glob(os.path.join(d, "*.det.json"))):
        det = json.load(open(f))
        verdict = os.path.basename(f).split("-")[0]
        fr = frames(det)
        key = verdict + " | " + " | ".join(fr[:4])
        groups.setdefault(key, []).append(f.replace(".det.json", ".json"))

for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1])):
    print(f"{len(v):4d}  {k}")
    print(f"      e.g. {v[0]}")
