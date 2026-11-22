#!/usr/bin/env python3
import sys, statistics as st
f, base = sys.argv[1:3]
keys = ["mp", "mt", "h16", "ht", "pipe", "q8", "w8", "q4", "w4"]  # all lower is better
v = {}
for l in open(f):
    p = l.split()
    if len(p) < 2 or "=" not in p[1]: print(l.strip()); continue
    if p[0] != "default" and "state=enabled" not in l: print("FALLBACK", p[0]); continue
    for kv in p[1:]:
        k, _, x = kv.partition("=")
        if k in keys and x: v.setdefault(p[0], {}).setdefault(k, []).append(float(x))
b = {k: st.median(x) for k, x in v[base].items()}
print(f"{'arm':8}" + "".join(f"{k:>8}" for k in keys) + "   (% vs " + base + ", + is better)")
for a in sorted(v):
    print(f"{a:8}" + "".join(f"{(b[k]-st.median(v[a][k]))/b[k]*100:+7.1f}%" if k in v[a] else "       ?" for k in keys) + f"  n={len(v[a].get('mp', []))}")
