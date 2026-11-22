#!/bin/bash
# Night round 1: after the pv4ck validation, build and screen four ideas on pv4ck.
S=$(dirname "$0")
cd $S
until ./vm.sh 'grep -q "all done" ~/lertick-exp/val3-run.log' 2>/dev/null; do sleep 60; done
B="WAKER_PREEMPT=3 WP_LEARN=2 WP_POLICY=5 WP_PCPU=1 WP_NOSYNC=1 PREEMPT_PCPU=1 STEAL_PCPU=1 PQ_FAIR=1 WP_PQ=1 PQ_EXT=2 CPUQ_FAIR=1 WP_WORK=1 WQ_LEARN=1 KEEP_PCPU=1 CPUQ_CACHE=1"
NOSTAT=1 ./mk.sh na1 $B WP_CTX=5 </dev/null | tail -1 || exit 1
NOSTAT=1 ./mk.sh na2 $(echo $B | sed 's/PQ_FAIR=1/PQ_FAIR=2/') </dev/null | tail -1 || exit 1
NOSTAT=1 ./mk.sh na3 $(echo $B | sed 's/PQ_FAIR=1/PQ_FAIR=2/') WP_CTX=5 </dev/null | tail -1 || exit 1
NOSTAT=1 ./mk.sh na4 $B MQ_LEARN=1 MQ_STEP=1 MQ_DRIFT=1 MQ_CACHE=1 </dev/null | tail -1 || exit 1
echo "== night round 1 $(TZ=America/Los_Angeles date +%T)"
./regress.sh pv4ck na1 na2 na3 na4 2>&1
echo "== 3 hogs on 2 CPUs"
./vm.sh "cd ~/lertick-exp && for r in 1 2; do ./fair32.sh default pv4ck=scx/pv4ck/scx_lertick,--min-train-samples,2 na2=scx/na2/scx_lertick,--min-train-samples,2 na3=scx/na3/scx_lertick,--min-train-samples,2 na4=scx/na4/scx_lertick,--min-train-samples,2; done" </dev/null 2>&1 | grep -v "No such"
echo "== scaling"
./vm.sh "cd ~/lertick-exp && ./scale.sh 3 default pv4ck=scx/pv4ck/scx_lertick,--min-train-samples,2 na1=scx/na1/scx_lertick,--min-train-samples,2 na2=scx/na2/scx_lertick,--min-train-samples,2 na4=scx/na4/scx_lertick,--min-train-samples,2" > scale4.txt </dev/null
python3 - scale4.txt <<'PY'
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
          "| messaging vs Linux:", " ".join(f"{n}cpu={(lin['m%d'%n]-st.median(d[a]['m%d'%n]))/lin['m%d'%n]*100:+.0f}%" for n in (1,2,4,6,8)))
PY
echo "== done $(TZ=America/Los_Angeles date +%T)"
