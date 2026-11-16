# Usage: python3 analyze.py <dir holding runs/>; needs the full per-trial
# workload.jsonl and wake.jsonl, which stay in the guest (trials.tar.gz keeps trial.json).
import json,glob,sys,os,statistics as st
root=sys.argv[1]
def pct(v,p):
    v=sorted(v); return v[min(len(v)-1,int(p*len(v)))] if v else float('nan')
arms=[('EEVDF','base-default'),('fixed','base-fixed'),('online','base-online'),('A','A-online'),('B','B-online'),('C','C-online')]
res={}
for name,arm in arms:
    for d in sorted(glob.glob(f'{root}/runs/{arm}/r0*-*')):
        if not os.path.isdir(d): continue
        wl=os.path.basename(d).split('-')[1]
        m={}
        roles={}
        for l in open(d+'/workload.jsonl'):
            if not l.strip(): continue
            e=json.loads(l)
            if e['event']=='worker': roles[e['tid']]=(e['role'],e['burst_target_ns']//1000,e['max_progress_gap_ns'])
            elif e['event']=='sample' and e['role']=='burst':
                k=roles[e['tid']][1]; m.setdefault(f'compl{k}',[]).append(e['completion_ns'])
        m['cpugap']=[max([g for r,_,g in roles.values() if r=='cpu'] or [0])]
        if os.path.exists(d+'/wake.jsonl'):
            for l in open(d+'/wake.jsonl'):
                if not l.strip(): continue
                e=json.loads(l)
                if e.get('event')=='wake_run' and e['tid'] in roles:
                    m.setdefault(f'wake{roles[e["tid"]][1]}',[]).append(e['latency_ns'])
        t=json.load(open(d+'/trial.json'))
        row={k:(pct(v,.99) if k!='cpugap' else v[0]) for k,v in m.items()}
        row['thr']=t['throughput_units_per_s']
        res.setdefault((wl,name),[]).append(row)
wls=sorted({k[0] for k in res})
for wl in wls:
    keys=sorted({k for name in [a for a,_ in arms] for r in res.get((wl,name),[]) for k in r})
    print(f'\n== {wl} (median of per-trial p99, ms; thr in M)')
    print('arm     n '+' '.join(f'{k:>10}' for k in keys))
    for name,_ in arms:
        rs=res.get((wl,name),[])
        if not rs: continue
        vals=[]
        for k in keys:
            v=[r[k] for r in rs if k in r]
            x=st.median(v) if v else float('nan')
            vals.append(f'{x/1e6:10.2f}')
        print(f'{name:7} {len(rs)} '+' '.join(vals))
