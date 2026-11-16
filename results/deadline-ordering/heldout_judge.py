# Summarize heldout.sh output against EEVDF. Usage: heldout_judge.py DIR
import glob,os,re,statistics as st,sys
root=sys.argv[1]
def num(path,pattern):
    m=re.findall(pattern,open(path).read())
    return float(m[-1]) if m else None
def schbench(path,section,pct):
    t=open(path).read(); starts=[m.end() for m in re.finditer(section+r' percentiles',t)]
    if not starts: return None
    m=re.search(r'\*?\s*'+re.escape(pct)+r':\s*(\d+)',t[starts[-1]:]); return float(m.group(1)) if m else None
# metric: (file, extractor, higher_is_better)
M={'msg-proc s':('msg-proc.txt',lambda p:num(p,r'Total time:\s*([\d.]+)'),False),
   'msg-thread s':('msg-thread.txt',lambda p:num(p,r'Total time:\s*([\d.]+)'),False),
   'pipe us/op':('pipe.txt',lambda p:num(p,r'([\d.]+)\s*usecs/op'),False),
   'sb-m4t2 wake p99':('sb-m4t2.txt',lambda p:schbench(p,'Wakeup Latencies','99.0th'),False),
   'sb-m4t2 req p99':('sb-m4t2.txt',lambda p:schbench(p,'Request Latencies','99.0th'),False),
   'sb-m4t2 rps':('sb-m4t2.txt',lambda p:schbench(p,'RPS','50.0th'),True),
   'sb-m1t1 wake p99':('sb-m1t1.txt',lambda p:schbench(p,'Wakeup Latencies','99.0th'),False),
   'sb-m1t1 req p99':('sb-m1t1.txt',lambda p:schbench(p,'Request Latencies','99.0th'),False),
   'hb-thread s':('hb-thread.txt',lambda p:num(p,r'Time:\s*([\d.]+)'),False),
   'hb-g16 s':('hb-g16.txt',lambda p:num(p,r'Time:\s*([\d.]+)'),False),
   'kbuild s':('kbuild.txt',lambda p:num(p,r'elapsed\s*([\d.]+)'),False)}
data={}; fb={}
for d in glob.glob(root+'/*-*'):
    if not os.path.exists(d+'/done'): continue
    arm=os.path.basename(d).split('-',1)[1]
    if os.path.exists(d+'/fallback'): fb[arm]=fb.get(arm,0)+1
    for k,(f,fn,_) in M.items():
        if os.path.exists(d+'/'+f):
            v=fn(d+'/'+f)
            if v is not None: data.setdefault((arm,k),[]).append(v)
arms=sorted({a for a,_ in data},key=lambda a:(a!='default',a))
med=lambda a,k: st.median(data[(a,k)]) if (a,k) in data else None
print(f"{'test':18}"+''.join(f'{a:>13}' for a in arms))
for k in M: print(f'{k:18}'+''.join(f'{(med(a,k) or 0):13.4g}' for a in arms))
print('\nchange vs EEVDF (negative = better), verdict at 5%')
for a in arms:
    if a=='default': continue
    w=t=l=0; parts=[]
    for k,(_,_,hib) in M.items():
        x,y=med(a,k),med('default',k)
        if not x or not y: continue
        c=(y/x-1) if hib else (x/y-1)
        v='W' if c<=-0.05 else 'L' if c>0.05 else 'T'
        w+=v=='W'; t+=v=='T'; l+=v=='L'; parts.append(f'{k}={c:+.0%}{v}')
    print(f"{a:10} win {w} tie {t} loss {l}  fallbacks {fb.get(a,0)}  n={len(data.get((a,'msg-proc s'),[]))}")
    print('   '+'  '.join(parts))
