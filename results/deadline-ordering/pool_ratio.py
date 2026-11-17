# README numbers for the current version: Lertick against EEVDF from one
# session (LERTICK_SESSION, arm LERTICK_ARM), and against each sched_ext
# scheduler through EEVDF: per session, each scheduler's median over EEVDF's
# median in that session; the median of those ratios over every session that
# ran the scheduler is set against Lertick's ratio. Run from a directory
# holding the extracted heldout/* and kbuild-long/* sets. Prints the lists for
# make_chart.py in its row order (positive is better for Lertick).
import os, re, statistics as st, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.argv = [sys.argv[0], 'x']
ns = {}
exec(open(os.path.join(here, 'heldout_judge.py')).read().split('data={}; fb={}')[0], ns)
M = ns['M']
ORDER = ['msg-proc s', 'msg-thread s', 'pipe us/op', 'hb-thread s', 'hb-g16 s',
         'sb-m1t1 req p99', 'sb-m1t1 wake p99', 'sb-m4t2 req p99', 'sb-m4t2 wake p99',
         'sb-m4t2 rps', 'kbuild s']
LT_SESSION = os.environ.get('LERTICK_SESSION', 'w2')
LT_ARM = os.environ.get('LERTICK_ARM', 'W2')
OTHERS = {'bpfland': ['cpu{c}', 'eve1-cpu{c}'],
          'lavd': ['cpu{c}-lavd', 'eve1-cpu{c}'],
          'p2dq': ['cpu{c}-more', 'eve1-cpu{c}'],
          'flash': ['cpu{c}-more', 'eve1-cpu{c}']}


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


def ratios(session, arm):
    """arm's median over EEVDF's median per measure, in one session."""
    h, kb = heldout('heldout/' + session), kbuild('kbuild-long/' + session)
    out = {}
    for k in ORDER:
        x, y = (kb.get(arm), kb.get('default')) if k == 'kbuild s' else (h.get((arm, k)), h.get(('default', k)))
        if x and y:
            out[k] = x / y
    return out


def pct(rl, ro, hib):
    r = rl / ro  # Lertick over the other scheduler
    return round(((r - 1) if hib else (1 - r)) * 100, 1)


for cpu in (4, 8):
    lt = ratios(f'{LT_SESSION}-cpu{cpu}', LT_ARM)
    print(f'{cpu}: "EEVDF": {[pct(lt[k], 1.0, M[k][2]) for k in ORDER]}')
    for o, sessions in OTHERS.items():
        per = [ratios(s.format(c=cpu), o) for s in sessions]
        row = []
        for k in ORDER:
            rs = [r[k] for r in per if k in r]
            row.append(pct(lt[k], st.median(rs), M[k][2]) if rs else None)
        print(f'{cpu}: "{o}" ({len(per)} sessions): {row}')
