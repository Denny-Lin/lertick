# README numbers from same-session sets. Against EEVDF: the median over every
# session of Lertick's median over EEVDF's median. Against each sched_ext
# scheduler: Lertick's median over that scheduler's median in the brawl
# session. All from heldout.sh sets (kernel compile = kernel/sched rebuild).
# Usage: brawl_ratio.py ROOT where ROOT holds SESSION-cpuN directories;
# LERTICK_SESSIONS="name:arm,..." lists the sessions and the Lertick arm.
import os, statistics as st, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.argv = [sys.argv[0], 'x'] + sys.argv[1:]
ns = {}
exec(open(os.path.join(here, 'heldout_judge.py')).read().split('data={}; fb={}')[0], ns)
M = ns['M']
ORDER = ['msg-proc s', 'msg-thread s', 'pipe us/op', 'hb-thread s', 'hb-g16 s',
         'sb-m1t1 req p99', 'sb-m1t1 wake p99', 'sb-m4t2 req p99', 'sb-m4t2 wake p99',
         'sb-m4t2 rps', 'kbuild s']
root = sys.argv[2]
sessions = [s.split(':') for s in os.environ['LERTICK_SESSIONS'].split(',')]
OTHERS = ['bpfland', 'lavd', 'p2dq', 'flash']


def medians(d):
    data = {}
    for r in sorted(os.listdir(d)):
        p = os.path.join(d, r)
        if '-' not in r or not os.path.exists(p + '/done') or os.path.exists(p + '/fallback'):
            continue
        arm = r.split('-', 1)[1]
        for k, (f, fn, _) in M.items():
            if os.path.exists(p + '/' + f):
                v = fn(p + '/' + f)
                if v is not None:
                    data.setdefault((arm, k), []).append(v)
    return {key: st.median(v) for key, v in data.items()}


def pct(r, hib):
    return round(((r - 1) if hib else (1 - r)) * 100, 1)


for cpu in (4, 8):
    per = []
    for name, arm in sessions:
        d = os.path.join(root, f'{name}-cpu{cpu}')
        if not os.path.isdir(d):
            continue
        m = medians(d)
        per.append({k: m[(arm, k)] / m[('default', k)] for k in ORDER
                    if (arm, k) in m and ('default', k) in m})
    print(f'{cpu}: sessions {len(per)}')
    print(f'{cpu}: "EEVDF": {[pct(st.median([r[k] for r in per if k in r]), M[k][2]) for k in ORDER]}')
    m = medians(os.path.join(root, f'brawl-cpu{cpu}'))
    for o in OTHERS:
        row = [pct(m[('lertick', k)] / m[(o, k)], M[k][2]) if (o, k) in m and ('lertick', k) in m else None for k in ORDER]
        print(f'{cpu}: "{o}": {row}')
