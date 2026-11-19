# cmp.py DIR ARM...: median of each measure per arm, % vs EEVDF (positive = better).
import os, sys, statistics as st
ns = {}; argv = sys.argv; sys.argv = [argv[0], 'x']
exec(open(os.path.expanduser('~/lertick-exp/heldout_judge.py')).read().split('data={}; fb={}')[0], ns)
M = ns['M']; d = argv[1]; arms = argv[2:]
ORDER=['msg-proc s','msg-thread s','pipe us/op','hb-thread s','hb-g16 s','sb-m1t1 req p99','sb-m1t1 wake p99','sb-m4t2 req p99','sb-m4t2 wake p99','sb-m4t2 rps','kbuild s']
data = {}
for r in sorted(os.listdir(d)):
    p = os.path.join(d, r)
    if '-' not in r or not os.path.exists(p+'/done') or os.path.exists(p+'/fallback'): continue
    a = r.split('-', 1)[1]
    for k, (f, fn, _) in M.items():
        if os.path.exists(p+'/'+f):
            v = fn(p+'/'+f)
            if v is not None: data.setdefault((a, k), []).append(v)
pct = lambda r, hib: ((r-1) if hib else (1-r))*100
print('%-18s' % 'measure' + ''.join('%14s' % a for a in arms), '   n')
for k in ORDER:
    b = st.median(data[('default', k)])
    row = '%-18s' % k
    for a in arms:
        v = data.get((a, k))
        row += '%14s' % ('%+.1f%%' % pct(st.median(v)/b, M[k][2]) if v else '-')
    print(row, '  ', len(data[('default', k)]))
