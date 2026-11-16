# Apply the README's criteria to confirm/summary.jsonl (one row per trial).
# Usage: python3 judge.py summary.jsonl
import json,sys,statistics as st
rows=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]
arms=['base-default','base-fixed','base-online','B-online','base-scx_bpfland']
med=lambda arm,wl,k:(lambda v:st.median(v) if v else None)([r[k] for r in rows if r['arm']==arm and r['workload']==wl and r.get(k) is not None])
n=lambda arm,wl:sum(1 for r in rows if r['arm']==arm and r['workload']==wl)
for wl in ['mixed','multicore','shortburst','wakeheavy']:
    classes=sorted({k for r in rows if r['workload']==wl for k in r if k.startswith(('compl','wake'))})
    print(f'\n== {wl}  (median of 10 per-trial p99, ms)')
    print(f"{'arm':17}{'n':>3} "+' '.join(f'{c:>10}' for c in classes)+f"{'gap':>8}{'thr(M)':>9}")
    for a in arms:
        if not n(a,wl): continue
        print(f'{a:17}{n(a,wl):3} '+' '.join(f'{med(a,wl,c)/1e6:10.2f}' for c in classes)+f"{med(a,wl,'worker_gap_ns')/1e6:8.1f}{med(a,wl,'throughput')/1e6:9.1f}")
    e,b,o='base-default','B-online','base-online'
    if not (n(e,wl) and n(b,wl)): continue
    ch={c:med(b,wl,c)/med(e,wl,c)-1 for c in classes}
    c1=all(v<=0.05 for v in ch.values()) and any(v<=-0.10 for v in ch.values())
    gap=med(b,wl,'worker_gap_ns')/med(e,wl,'worker_gap_ns'); c2=gap<=1.5
    thr=med(b,wl,'throughput')/med(e,wl,'throughput')-1; c3=thr>=-0.02
    vso={c:med(b,wl,c)/med(o,wl,c)-1 for c in classes} if n(o,wl) else {}
    c4=all(v<=0.05 for v in vso.values())
    print('B vs EEVDF: '+', '.join(f'{c} {v:+.0%}' for c,v in ch.items())+f'; gap x{gap:.2f}; thr {thr:+.1%}')
    print('B vs online: '+', '.join(f'{c} {v:+.0%}' for c,v in vso.items()))
    print(f'criteria: classes={c1} gap={c2} throughput={c3} vs-online={c4} -> {"WIN" if c1 and c2 and c3 else "NO WIN"}')
