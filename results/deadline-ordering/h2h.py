# Head-to-head: Lertick (rel-on) against other arms in the same held-out sessions,
# dropping repetitions where an arm fell back, and using kbuild-long for kernel compile.
import os,re,statistics as st,sys
sys.argv=[sys.argv[0],'x']
src=open('/Users/hungyu_lin/Desktop/Github/lertick/.worktrees/deadline-ordering/results/deadline-ordering/heldout_judge.py').read()
ns={}; exec(src.split('data={}; fb={}')[0],ns)
M=ns['M']
def load(root):
    data={}
    for d in sorted(os.listdir(root)):
        p=os.path.join(root,d)
        if '-' not in d or not os.path.exists(p+'/done') or os.path.exists(p+'/fallback'): continue
        arm=d.split('-',1)[1]
        for k,(f,fn,_) in M.items():
            if k=='kbuild s' or not os.path.exists(p+'/'+f): continue
            v=fn(p+'/'+f)
            if v is not None: data.setdefault((arm,k),[]).append(v)
    return data
def kbl(root):
    d={}
    for l in open(root+'/progress.log'):
        m=re.search(r'ok (\d)-(\S+) elapsed ([\d.]+)',l)
        if m: d.setdefault(m.group(2),[]).append(float(m.group(3)))
    return {a:st.median(v) for a,v in d.items()}
for cpu in (4,8):
    h=load(f'heldout/cpu{cpu}-more'); kb=kbl(f'kbuild-long/cpu{cpu}-more')
    print(f'{cpu} CPUs, rel-on reps kept:',len(h[('rel-on','msg-proc s')]))
    for other in ('p2dq','flash','default'):
        b=s_=w=0; parts=[]
        for k,(f,fn,hib) in M.items():
            if k=='kbuild s': lt,o=kb['rel-on'],kb[other]
            else: lt,o=st.median(h[('rel-on',k)]),st.median(h[(other,k)])
            pct=((lt/o-1) if hib else (1-lt/o))*100
            if pct>5: b+=1; v='B'
            elif pct<-5: w+=1; v='W'
            else: s_+=1; v='S'
            parts.append(f'{k}={pct:+.0f}{v}')
        print(f'  vs {other:7} better {b} same {s_} worse {w} | '+'  '.join(parts))
