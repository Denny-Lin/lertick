#!/usr/bin/env python3
# fx-rank.py FILE BASE: rank arms against BASE. An arm qualifies if no
# non-futex measure is >5% worse than BASE; qualified arms are sorted by futex gain.
import sys, statistics as st
f, base = sys.argv[1:3]
higher = {"f8": True, "f1": True, "rps": True, "sbw": False, "sbq": False, "sbq8": False, "rps8": True, "hb": False}
vals = {}
for line in open(f):
    p = line.split()
    if len(p) < 2 or "=" not in p[1]: continue
    if p[0] != "default" and "state=enabled" not in line:
        print("FALLBACK", p[0]); continue
    for kv in p[1:]:
        k, _, v = kv.partition("=")
        if k in higher and v:
            vals.setdefault(p[0], {}).setdefault(k, []).append(float(v))
b = {k: st.median(v) for k, v in vals[base].items()}
rows = []
for arm, kv in vals.items():
    if arm == base: continue
    r = {}
    for k, hi in higher.items():
        if k not in kv or k not in b: r[k] = None; continue
        m = st.median(kv[k]); r[k] = (m - b[k]) / b[k] * 100 if hi else (b[k] - m) / b[k] * 100
    worse = [k for k in ("rps", "sbw", "sbq", "sbq8", "rps8", "hb") if r[k] is None or r[k] < -5]
    fx = max(r["f8"] or -1e9, r["f1"] or -1e9)
    rows.append((not worse, (r["f8"] or 0) + (r["f1"] or 0), arm, r, worse))
rows.sort(key=lambda x: (x[0], x[1]), reverse=True)
print(f"base {base}: " + " ".join(f"{k}={b[k]:.0f}" for k in higher if k in b))
for i, (ok, sc, arm, r, worse) in enumerate(rows, 1):
    print(f"{i:2} {arm:6} {'ok ' if ok else 'BAD'} " + " ".join(f"{k}={r[k]:+.0f}%" if r[k] is not None else f"{k}=?" for k in higher) + (f" worse:{','.join(worse)}" if worse else ""))
