# Summarize eval.sh output. Usage: eval_judge.py DIR [INCUMBENT_ARM]
# Ratios are candidate / EEVDF (and / incumbent); for RPS the ratio is inverted so lower is better.
import glob,math,os,re,statistics as st,sys
root=sys.argv[1]; inc=sys.argv[2] if len(sys.argv)>2 else None
def schbench(path):
    text=open(path).read(); out={}
    for key,section,pct in (('wake','Wakeup Latencies','99.0th'),('req','Request Latencies','99.0th'),('rps','RPS','50.0th')):
        starts=[m.end() for m in re.finditer(section+r' percentiles',text)]
        if starts:
            m=re.search(r'\*?\s*'+re.escape(pct)+r':\s*(\d+)',text[starts[-1]:])
            if m: out[key]=float(m.group(1))
    return out
data={}; fallbacks={}
for d in glob.glob(root+'/*-*'):
    if not os.path.exists(d+'/done'): continue
    arm=os.path.basename(d).split('-',1)[1]
    if os.path.exists(d+'/fallback'): fallbacks[arm]=fallbacks.get(arm,0)+1
    for f in glob.glob(d+'/schbench-*.txt'):
        for k,v in schbench(f).items(): data.setdefault((arm,'sb-'+os.path.basename(f)[9:-4]+'-'+k),[]).append(v)
    for f in glob.glob(d+'/hackbench-*.txt'):
        m=re.search(r'Time:\s*([\d.]+)',open(f).read())
        if m: data.setdefault((arm,'hb-'+os.path.basename(f)[10:-4]),[]).append(float(m.group(1)))
arms=sorted({a for a,_ in data}); metrics=sorted({m for _,m in data})
med=lambda a,m: st.median(data[(a,m)]) if (a,m) in data else None
def ratio(a,b,m):
    x,y=med(a,m),med(b,m)
    if not x or not y: return None
    return y/x if m.endswith('rps') else x/y
print(f"{'metric':22}"+''.join(f'{a:>14}' for a in arms))
for m in metrics: print(f'{m:22}'+''.join(f'{med(a,m) or 0:14.4g}' for a in arms))
for base in ['default']+([inc] if inc else []):
    print(f'\nratio vs {base} (<1 better)')
    for a in arms:
        if a==base: continue
        r=[ratio(a,base,m) for m in metrics]; r=[x for x in r if x]
        g=math.exp(sum(math.log(x) for x in r)/len(r)) if r else float('nan')
        worst=max(zip([ratio(a,base,m) or 0 for m in metrics],metrics))
        print(f"{a:16} geomean {g:.3f}  worst {worst[1]} {worst[0]:.2f}  fallbacks {fallbacks.get(a,0)}  n={len(data.get((a,metrics[0]),[]))}")
        print('   '+' '.join(f'{m.replace("sb-","").replace("hb-","hb:")}={ratio(a,base,m) or 0:.2f}' for m in metrics))
