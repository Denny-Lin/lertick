# README numbers for the deadline-ordering build: against EEVDF, the median
# over the sessions x4 (arm d50) and final5 (arm v5) of its median over
# EEVDF's; against the sched_ext schedulers, that ratio over each one's ratio
# to EEVDF in the brawl session. Usage: ROOT=DIR dl_ratio.py
import os, statistics as st, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.argv=[sys.argv[0],'x']
ns={}; exec(open(os.path.join(here, 'heldout_judge.py')).read().split('data={}; fb={}')[0], ns)
M=ns['M']
ORDER=['msg-proc s','msg-thread s','pipe us/op','hb-thread s','hb-g16 s','sb-m1t1 req p99','sb-m1t1 wake p99','sb-m4t2 req p99','sb-m4t2 wake p99','sb-m4t2 rps','kbuild s']
root=os.environ['ROOT']
def medians(d):
    data={}
    for r in sorted(os.listdir(d)):
        p=os.path.join(d,r)
        if '-' not in r or not os.path.exists(p+'/done') or os.path.exists(p+'/fallback'): continue
        arm=r.split('-',1)[1]
        for k,(f,fn,_) in M.items():
            if os.path.exists(p+'/'+f):
                v=fn(p+'/'+f)
                if v is not None: data.setdefault((arm,k),[]).append(v)
    return {k:st.median(v) for k,v in data.items()}
pct=lambda r,hib: round(((r-1) if hib else (1-r))*100,1)
for cpu in (4,8):
    per=[]
    for name,arm in [('x4','d50'),('final5','v5')]:
        m=medians(os.path.join(root,f'{name}-cpu{cpu}'))
        per.append({k:m[(arm,k)]/m[('default',k)] for k in ORDER})
    lt={k:st.median([r[k] for r in per]) for k in ORDER}
    print(f'{cpu}: "EEVDF": {[pct(lt[k],M[k][2]) for k in ORDER]}')
    m=medians(os.path.join(root,f'brawl-cpu{cpu}'))
    for o in ['bpfland','lavd','p2dq','flash']:
        print(f'{cpu}: "{o}": {[pct(lt[k]/(m[(o,k)]/m[("default",k)]),M[k][2]) for k in ORDER]}')
