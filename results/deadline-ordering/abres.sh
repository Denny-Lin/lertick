#!/bin/bash
# abres.sh LABEL NARMS REF: wait for msg-run LABEL, then paired results vs EEVDF and vs REF.
S=/private/tmp/claude-501/-Users-hungyu-lin-Desktop-Github/5b54c50f-58a7-426f-be0a-70295def418e/scratchpad
until $S/vm.sh "test -f ~/lertick-exp/msg/$1/done" 2>/dev/null; do sleep 60; done
$S/vm.sh "cat ~/lertick-exp/msg/$1/times.txt" > $S/$1.txt
$S/vm.sh "cat ~/lertick-exp/msg/$1/fallback.txt 2>/dev/null"
python3 $S/paired.py $S/$1.txt $2 5
python3 - $S/$1.txt $2 $3 <<'PY'
import sys,statistics as st
rows=[l.split() for l in open(sys.argv[1]) if len(l.split())==2]; n=int(sys.argv[2]); ref=sys.argv[3]
blocks=[]
for a,t in rows:
    if blocks and blocks[-1][0]==a and len(blocks[-1][1])<5: blocks[-1][1].append(float(t))
    else: blocks.append((a,[float(t)]))
R=[dict((a,st.median(v)) for a,v in blocks[i:i+n]) for i in range(0,len(blocks),n)]
for a in sorted(R[0]):
    if a in ('default',ref): continue
    r=sorted(x[a]/x[ref] for x in R if a in x and ref in x)
    print(a,'vs',ref,': median %+.1f%%, faster in %d/%d rounds'%(100*(st.median(r)-1),sum(v<1 for v in r),len(r)))
PY
