#!/bin/bash
# After the pe2c validation: build the improvements and screen them.
S=$(dirname "$0")
cd $S
until ./vm.sh 'grep -q "all done" ~/lertick-exp/val-run.log' 2>/dev/null; do sleep 60; done
B="WAKER_PREEMPT=3 WP_LEARN=1 WP_PCPU=1 WP_NOSYNC=1 PREEMPT_PCPU=1 STEAL_PCPU=1 PQ_FAIR=1 WP_PQ=1 PQ_EXT=2 CPUQ_FAIR=1"
NOSTAT=1 ./mk.sh pg1 $B WP_POLICY=5 CPUQ_CACHE=1 </dev/null | tail -1 || exit 1
NOSTAT=1 ./mk.sh pg2 $B WP_POLICY=8 </dev/null | tail -1 || exit 1
NOSTAT=1 ./mk.sh pg3 $B WP_POLICY=8 CPUQ_CACHE=1 </dev/null | tail -1 || exit 1
A=""; for a in pe2c pg1 pg2 pg3; do A="$A $a=scx/$a/scx_lertick,--min-train-samples,2"; done
echo "== futex 8 CPU"; ./vm.sh "cd ~/lertick-exp && ./fx-cs.sh 3 $A" </dev/null 2>&1 | grep -v "No such" | sort
echo "== 4-CPU set"; ./vm.sh "cd ~/lertick-exp && ./fx4-screen.sh 3 fx0n=scx/fx0n/scx_lertick,--min-train-samples,2 $A" > imp-s4.txt </dev/null; python3 fx4-rank.py imp-s4.txt fx0n
echo "== nice gate"; for a in pg1 pg2 pg3; do ./vm.sh "cd ~/lertick-exp && REPS=2 ./gate.sh $a scx/$a/scx_lertick --min-train-samples 2 2>&1 | grep -E 'fairness|passed'" </dev/null; done
echo "== scaling 1-8 CPUs"
./vm.sh "cd ~/lertick-exp && ./scale.sh 3 default fx0n=scx/fx0n/scx_lertick,--min-train-samples,2 pe2c=scx/pe2c/scx_lertick,--min-train-samples,2 pg3=scx/pg3/scx_lertick,--min-train-samples,2" > scale.txt </dev/null
python3 - scale.txt <<'PY'
import sys, statistics as st
d = {}
for l in open(sys.argv[1]):
    p = l.split()
    if len(p) < 3 or "=" not in p[1]: continue
    for kv in p[1:]:
        k, _, v = kv.partition("=")
        if k[0] in "fm" and v: d.setdefault(p[0], {}).setdefault(k, []).append(float(v))
lin = {k: st.median(v) for k, v in d["default"].items()}
for a in d:
    print(a, "futex % of Linux:", " ".join(f"{n}cpu={st.median(d[a]['f%d'%n])/lin['f%d'%n]*100:.0f}%" for n in (1,2,4,6,8)),
          "| messaging time vs Linux:", " ".join(f"{n}cpu={(lin['m%d'%n]-st.median(d[a]['m%d'%n]))/lin['m%d'%n]*100:+.0f}%" for n in (1,2,4,6,8)))
PY
