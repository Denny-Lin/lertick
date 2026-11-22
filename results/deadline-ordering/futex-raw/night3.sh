#!/bin/bash
S=$(dirname "$0")
cd $S
B="WAKER_PREEMPT=3 WP_LEARN=2 WP_POLICY=5 WP_PCPU=1 WP_NOSYNC=1 PREEMPT_PCPU=1 STEAL_PCPU=1 PQ_FAIR=1 PQ_EXT=2 CPUQ_FAIR=1 WP_WORK=1 KEEP_PCPU=1 CPUQ_CACHE=1"
NOSTAT=1 ./mk.sh ac1 $B WP_PQ=0 WQ_LEARN=1 </dev/null | tail -1 || exit 1
NOSTAT=1 ./mk.sh ac3 $B WP_PQ=0 WQ_LEARN=0 </dev/null | tail -1 || exit 1
echo "== night round 3 $(TZ=America/Los_Angeles date +%T)"
./regress.sh pv4ck ac1 ac3 2>&1
echo "== 3 hogs on 2 CPUs"
./vm.sh "cd ~/lertick-exp && ./fair32.sh default ac1=scx/ac1/scx_lertick,--min-train-samples,2 ac3=scx/ac3/scx_lertick,--min-train-samples,2" </dev/null 2>&1 | grep -v "No such"
echo "== scaling"
./vm.sh "cd ~/lertick-exp && ./scale.sh 3 default pv4ck=scx/pv4ck/scx_lertick,--min-train-samples,2 ac1=scx/ac1/scx_lertick,--min-train-samples,2 ac3=scx/ac3/scx_lertick,--min-train-samples,2" > scale6.txt </dev/null
python3 - scale6.txt <<'PY'
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
./vm.sh 'sudo journalctl -k --since "-50 min" | grep -E "failed to run|runtime error" | head -5' </dev/null
echo "== done $(TZ=America/Los_Angeles date +%T)"
