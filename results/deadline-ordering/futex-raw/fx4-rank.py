#!/usr/bin/env python3
import sys, statistics as st
f, base = sys.argv[1:3]
hi = {"f4": True, "f1": True, "q1": False, "w1": False, "q2": False, "w2": False, "rps": True, "hb": False}
v = {}
for l in open(f):
    p = l.split()
    if len(p) < 2: continue
    if "state=enabled" not in l and p[0] != "default": print("FALLBACK", p[0]); continue
    for kv in p[1:]:
        k, _, x = kv.partition("=")
        if k in hi and x: v.setdefault(p[0], {}).setdefault(k, []).append(float(x))
b = {k: st.median(x) for k, x in v[base].items()}
rows = []
for a, kv in v.items():
    if a == base: continue
    r = {k: ((st.median(kv[k]) - b[k]) / b[k] * 100 if hi[k] else (b[k] - st.median(kv[k])) / b[k] * 100) if k in kv and k in b else None for k in hi}
    worse = [k for k in ("q1", "w1", "q2", "w2", "rps", "hb") if r[k] is None or r[k] < -5]
    rows.append((not worse, (r["f4"] or 0), a, r, worse))
rows.sort(key=lambda x: (x[0], x[1]), reverse=True)
print("base", base, " ".join(f"{k}={b[k]:.0f}" for k in hi if k in b))
for ok, sc, a, r, w in rows:
    print(f"{a:6} {'ok ' if ok else 'BAD'} " + " ".join(f"{k}={r[k]:+.0f}%" if r[k] is not None else f"{k}=?" for k in hi) + (f" worse:{','.join(w)}" if w else ""))
