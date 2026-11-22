#!/bin/bash
# Ablation of pv4ck: one part off per build.
S=$(dirname "$0")
cd $S
B="WAKER_PREEMPT=3 WP_LEARN=2 WP_POLICY=5 WP_PCPU=1 WP_NOSYNC=1 PREEMPT_PCPU=1 STEAL_PCPU=1 PQ_FAIR=1 WP_PQ=1 PQ_EXT=2 CPUQ_FAIR=1 WP_WORK=1 WQ_LEARN=1 KEEP_PCPU=1 CPUQ_CACHE=1"
mk() { n=$1; shift; NOSTAT=1 ./mk.sh $n "$@" </dev/null | tail -1 || exit 1; }
mk ab1 $(echo $B | sed 's/WAKER_PREEMPT=3/WAKER_PREEMPT=0/; s/WP_LEARN=2/WP_LEARN=0/')
mk ab2 $(echo $B | sed 's/WQ_LEARN=1/WQ_LEARN=0/')
mk ab3 $(echo $B | sed 's/WP_PQ=1/WP_PQ=0/; s/WQ_LEARN=1/WQ_LEARN=0/')
mk ab4 $(echo $B | sed 's/PREEMPT_PCPU=1/PREEMPT_PCPU=0/; s/WP_PQ=1/WP_PQ=0/; s/WQ_LEARN=1/WQ_LEARN=0/')
mk ab5 $(echo $B | sed 's/WP_WORK=1/WP_WORK=0/')
echo "== ablation $(TZ=America/Los_Angeles date +%T)"
A="default"; for a in pv4ck ab1 ab2 ab3 ab4 ab5; do A="$A $a=scx/$a/scx_lertick,--min-train-samples,2"; done
./vm.sh "cd ~/lertick-exp && ./fx4-screen.sh 4 $A" > abl-s4.txt </dev/null; python3 fx4-rank.py abl-s4.txt default
./vm.sh "cd ~/lertick-exp && ./fix-screen.sh 4 $A" </dev/null 2>&1 | grep -v "No such" > abl-fix.txt
python3 - abl-fix.txt <<'PY'
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
echo "== done $(TZ=America/Los_Angeles date +%T)"
