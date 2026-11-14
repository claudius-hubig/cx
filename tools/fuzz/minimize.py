#!/usr/bin/env python3
"""ddmin a failing e2e case over its steps (then over key chunks within steps).
usage: minimize.py CX CASE.json NEEDLE OUT.json
NEEDLE: substring that must appear in verdict+details (e.g. 'cannot make absolute')."""
import json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import e2e_fuzz as E

cx, src, needle, dst = sys.argv[1:5]
case = json.load(open(src))
work = os.path.join(os.path.dirname(os.path.abspath(dst)), "minwork")
runs = 0


def fails(steps):
    global runs
    runs += 1
    c = dict(case, steps=steps)
    for attempt in range(2):  # timing-dependent cases: allow one retry
        v, d = E.run_case(cx, c, work)
        blob = v + json.dumps(d)
        if needle in blob:
            return True
    return False


def ddmin(items):
    n = 2
    while len(items) >= 2:
        chunk = max(1, len(items) // n)
        subsets = [items[i:i + chunk] for i in range(0, len(items), chunk)]
        reduced = False
        for i in range(len(subsets)):
            complement = [x for j, s in enumerate(subsets) if j != i for x in s]
            if fails(complement):
                items, n, reduced = complement, max(n - 1, 2), True
                print(f"  [{runs}] -> {len(items)} steps", flush=True)
                break
        if not reduced:
            if n >= len(items):
                break
            n = min(len(items), n * 2)
    return items


steps = case["steps"]
if not fails(steps):
    print("does not reproduce")
    sys.exit(1)
steps = ddmin(steps)
# second pass: shrink inside each key step by characters
for idx in range(len(steps)):
    st = steps[idx]
    if "keys" not in st or len(st["keys"]) < 2:
        continue
    chars = list(st["keys"])

    def f2(cs, idx=idx):
        s2 = list(steps)
        s2[idx] = dict(st, keys="".join(cs))
        return fails(s2)

    n = 2
    while len(chars) >= 2:
        chunk = max(1, len(chars) // n)
        subs = [chars[i:i + chunk] for i in range(0, len(chars), chunk)]
        red = False
        for i in range(len(subs)):
            comp = [x for j, s in enumerate(subs) if j != i for x in s]
            if comp and f2(comp):
                chars, n, red = comp, max(n - 1, 2), True
                break
        if not red:
            if n >= len(chars):
                break
            n = min(len(chars), n * 2)
    steps[idx] = dict(st, keys="".join(chars))
json.dump(dict(case, steps=steps), open(dst, "w"))
print(f"minimized to {len(steps)} steps after {runs} runs:")
for s in steps:
    print("  ", repr(s))
