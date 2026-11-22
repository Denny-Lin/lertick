#!/bin/bash
# regress.sh ARM...: the fixed regression set, every arm against EEVDF (default is added).
S=$(dirname "$0")
cd $S
A="default"; for a in "$@"; do A="$A $a=scx/$a/scx_lertick,--min-train-samples,2"; done
tag=$(echo "$@" | tr ' ' '-')
./vm.sh "cd ~/lertick-exp && ./fix-screen.sh 5 $A" </dev/null 2>&1 | grep -v "No such" > reg-$tag-fix.txt
python3 - reg-$tag-fix.txt <<'PY'
import sys, statistics as st
d={}
for l in open(sys.argv[1]):
    p=l.split()
    if len(p)<2 or '=' not in p[1]: print(l.strip()); continue
    kv=dict(x.split('=') for x in p[1:]); d.setdefault(p[0],[]).append(kv)
L={k: st.median(float(x[k]) for x in d['default']) for k in ('mq','msg1','f8','f1')}
for a,v in sorted(d.items()):
    m=lambda k: st.median(float(x[k]) for x in v if x.get(k))
    print(f"{a:8} mq={m('mq')/L['mq']*100-100:+.0f}% msg1CPU={(L['msg1']-m('msg1'))/L['msg1']*100:+.0f}% futex8={m('f8')/L['f8']*100:.0f}%ofLinux futex1={m('f1')/L['f1']*100:.0f}%ofLinux n={len(v)}")
PY
echo "== 4-CPU set"; ./vm.sh "cd ~/lertick-exp && ./fx4-screen.sh 3 $A" > reg-$tag-s4.txt </dev/null; python3 fx4-rank.py reg-$tag-s4.txt default
echo "== 8-CPU switch loads"; ./vm.sh "cd ~/lertick-exp && ./mq-screen.sh 3 $A" > reg-$tag-s8.txt </dev/null; python3 mq-rank.py reg-$tag-s8.txt default
echo "== nice gate"; for a in "$@"; do ./vm.sh "cd ~/lertick-exp && REPS=2 ./gate.sh $a scx/$a/scx_lertick --min-train-samples 2 2>&1 | grep -E 'fairness|passed'" </dev/null; done
