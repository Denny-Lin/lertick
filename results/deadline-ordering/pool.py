# Pool Lertick against EEVDF over every held-out session that ran both arms.
# Run from a directory holding the extracted heldout/* sets. Each session gives
# one percentage per measure (median of its repetitions, fallback repetitions
# dropped); the verdict uses the median over sessions with the 5% threshold.
import os, statistics as st, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.argv = [sys.argv[0], 'x']
ns = {}
exec(open(os.path.join(here, 'heldout_judge.py')).read().split('data={}; fb={}')[0], ns)
M = ns['M']
SESSIONS = {4: [('cpu4', 'rel-on'), ('cpu4-lavd', 'rel-on'), ('cpu4-more', 'rel-on'),
                ('kick-cpu4', 'base'), ('t5-cpu4', 'base'), ('t5-cpu4', 't5')],
            8: [('cpu8', 'rel-on'), ('cpu8-lavd', 'rel-on'), ('cpu8-more', 'rel-on'),
                ('kick-cpu8', 'base'), ('t5-cpu8', 'base'), ('t5-cpu8', 't5')]}

def load(root):
    data = {}
    for d in sorted(os.listdir(root)):
        p = os.path.join(root, d)
        if '-' not in d or not os.path.exists(p + '/done') or os.path.exists(p + '/fallback'):
            continue
        arm = d.split('-', 1)[1]
        for k, (f, fn, _) in M.items():
            if os.path.exists(p + '/' + f):
                v = fn(p + '/' + f)
                if v is not None:
                    data.setdefault((arm, k), []).append(v)
    return data

for cpu, sessions in SESSIONS.items():
    print(f'== {cpu} CPUs: Lertick against EEVDF per session (+ = better), median over sessions')
    rows = {k: [] for k in M}
    for name, arm in sessions:
        d = load('heldout/' + name)
        for k, (f, fn, hib) in M.items():
            x, y = st.median(d[(arm, k)]), st.median(d[('default', k)])
            rows[k].append(((x / y - 1) if hib else (1 - x / y)) * 100)
    tally = {'better': 0, 'same': 0, 'worse': 0}
    for k, v in rows.items():
        m = st.median(v)
        verdict = 'better' if m > 5 else 'worse' if m < -5 else 'same'
        tally[verdict] += 1
        print(f'  {k:17} ' + ' '.join(f'{x:+5.0f}' for x in v) + f'  | median {m:+5.0f}% {verdict}')
    print('  pooled:', tally)
