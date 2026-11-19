import sys, statistics as st, collections
# times.txt: blocks of consecutive runs per arm; arms shuffled within each round.
rows=[l.split() for l in open(sys.argv[1])]
nargs=int(sys.argv[2])  # arms per round
blocks=[]
for a,t in rows:
    if blocks and blocks[-1][0]==a and len(blocks[-1][1])<int(sys.argv[3]): blocks[-1][1].append(float(t))
    else: blocks.append((a,[float(t)]))
rounds=[blocks[i:i+nargs] for i in range(0,len(blocks),nargs)]
rat=collections.defaultdict(list)
for r in rounds:
    m={a:st.median(v) for a,v in r}
    if 'default' not in m: continue
    for a,v in m.items(): rat[a].append(v/m['default'])
for a,v in sorted(rat.items()):
    if a=='default': continue
    v=sorted(v); n=len(v)
    print(f"{a}: median {100*(st.median(v)-1):+.1f}%  rounds slower than EEVDF {sum(x>1 for x in v)}/{n}  IQR {100*(v[n//4]-1):+.1f}..{100*(v[3*n//4]-1):+.1f}")
