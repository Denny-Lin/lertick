# Pool Lertick against every scheduler over all sessions that ran both, for the
# README charts. Run from a directory holding the extracted heldout/* and
# kbuild-long/* sets. Each session gives one percentage per measure (median of
# its repetitions, fallback repetitions dropped; kernel compile from
# kbuild-long); the result is the median over sessions, in make_chart.py order.
import os, re, statistics as st, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.argv = [sys.argv[0], 'x']
ns = {}
exec(open(os.path.join(here, 'heldout_judge.py')).read().split('data={}; fb={}')[0], ns)
M = ns['M']
ORDER = ['msg-proc s', 'msg-thread s', 'pipe us/op', 'hb-thread s', 'hb-g16 s',
         'sb-m1t1 req p99', 'sb-m1t1 wake p99', 'sb-m4t2 req p99', 'sb-m4t2 wake p99',
         'sb-m4t2 rps', 'kbuild s']
# (session, Lertick arm); the t5 sessions count once per Lertick build, as in pool.py.
SESSIONS = {c: [(f'cpu{c}', 'rel-on'), (f'cpu{c}-lavd', 'rel-on'), (f'cpu{c}-more', 'rel-on'),
                (f'kick-cpu{c}', 'base'), (f't5-cpu{c}', 'base'), (f't5-cpu{c}', 't5'),
                (f'eve1-cpu{c}', 't5')] for c in (4, 8)}
OTHERS = ['default', 'bpfland', 'lavd', 'p2dq', 'flash']


def heldout(root):
    data = {}
    for d in sorted(os.listdir(root)):
        p = os.path.join(root, d)
        if '-' not in d or not os.path.exists(p + '/done') or os.path.exists(p + '/fallback'):
            continue
        arm = d.split('-', 1)[1]
        for k, (f, fn, _) in M.items():
            if k != 'kbuild s' and os.path.exists(p + '/' + f):
                v = fn(p + '/' + f)
                if v is not None:
                    data.setdefault((arm, k), []).append(v)
    return {key: st.median(v) for key, v in data.items()}


def kbuild(root):
    d = {}
    if os.path.exists(root + '/progress.log'):
        for line in open(root + '/progress.log'):
            m = re.search(r'ok \d-(\S+) elapsed ([\d.]+)', line)
            if m:
                d.setdefault(m.group(1), []).append(float(m.group(2)))
    return {a: st.median(v) for a, v in d.items()}


for cpu, sessions in SESSIONS.items():
    rows = {o: {k: [] for k in ORDER} for o in OTHERS}
    for name, arm in sessions:
        h, kb = heldout('heldout/' + name), kbuild('kbuild-long/' + name)
        for o in OTHERS:
            for k in ORDER:
                hib = M[k][2]
                if k == 'kbuild s':
                    x, y = kb.get(arm), kb.get(o)
                else:
                    x, y = h.get((arm, k)), h.get((o, k))
                if x and y:
                    rows[o][k].append(((x / y - 1) if hib else (1 - x / y)) * 100)
    for o in OTHERS:
        n = sorted({len(v) for v in rows[o].values()})
        print(f'{cpu} {o:8} sessions {n}: [' +
              ', '.join(f'{st.median(rows[o][k]):.1f}' for k in ORDER) + ']')
