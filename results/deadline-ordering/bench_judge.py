# Parse bench/ (schbench, hackbench) and apply the README's criteria.
# Usage: python3 bench_judge.py <bench dir>
import glob,os,re,statistics as st,sys
root=sys.argv[1]
def schbench(path):
    text=open(path).read()
    out={}
    for key,section in (('wake','Wakeup Latencies'),('req','Request Latencies'),('rps','RPS')):
        blocks=[m.end() for m in re.finditer(section+r' percentiles',text)]
        if not blocks: continue
        tail=text[blocks[-1]:]
        pct='99.0th' if key!='rps' else '50.0th'
        m=re.search(r'\*?\s*'+re.escape(pct)+r':\s*(\d+)',tail)
        if m: out[key]=float(m.group(1))
    return out
data={}
for d in glob.glob(root+'/*-*'):
    if not os.path.isdir(d) or not os.path.exists(d+'/done'): continue
    arm=os.path.basename(d).split('-',1)[1]
    for f in glob.glob(d+'/schbench-*.txt'):
        cfg=os.path.basename(f)[9:-4]
        for k,v in schbench(f).items(): data.setdefault(('schbench '+cfg,k,arm),[]).append(v)
    for f in glob.glob(d+'/hackbench-*.txt'):
        m=re.search(r'Time:\s*([\d.]+)',open(f).read())
        if m: data.setdefault(('hackbench '+os.path.basename(f)[10:-4],'time',arm),[]).append(float(m.group(1)))
med=lambda c,k,a:st.median(data[(c,k,a)]) if (c,k,a) in data else None
for c in sorted({k[0] for k in data}):
    keys=['wake','req','rps'] if c.startswith('schbench') else ['time']
    print(f'\n== {c}')
    for a in ('default','fixed','online'):
        print(f"{a:8} n={len(data.get((c,keys[0],a),[]))} "+' '.join(f'{k}={med(c,k,a)}' for k in keys))
    for a in ('fixed','online'):
        ch={k:med(c,k,a)/med(c,k,'default')-1 for k in keys if med(c,k,a) is not None and med(c,k,'default')}
        if c.startswith('schbench'):
            win=ch['wake']<=-.10 and ch['req']<=.05 and ch['rps']>=-.02
            tie=abs(ch['wake'])<=.05 and abs(ch['req'])<=.05 and abs(ch['rps'])<=.02
        else:
            win=ch['time']<=-.10; tie=abs(ch['time'])<=.02
        print(f"{a} vs EEVDF: "+', '.join(f'{k} {v:+.1%}' for k,v in ch.items())+f" -> {'WIN' if win else 'NO LOSS' if tie else 'LOSS'}")
