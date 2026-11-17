#!/bin/bash
# Root-cgroup fix: build, root-cgroup test for EEVDF, the release build and the fix, then gate and cgroup weights.
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> rootfix-run.log; }
cp /tmp/port-rootfix.tgz /tmp/port.tgz; /tmp/scx-build.sh rootfix || { log "FAIL build"; exit 1; }
until grep -q " done$" long/nice-rel/progress.log 2>/dev/null; do sleep 15; done
pkill -f nicelong-run.sh; sleep 2; pgrep -f "nicelong.sh nice-b7" > /dev/null && { sudo pkill -INT -f "scx/b7/scx_lertick"; sleep 3; pkill -f nicelong.sh; }
for _ in $(seq 60); do [ "$(cat /sys/kernel/sched_ext/state)" = disabled ] && break; sleep 1; done
for r in 1 2; do
  ./rootcg.sh eevdf >> rootcg.txt 2>&1
  ./rootcg.sh rel scx/rel/scx_lertick >> rootcg.txt 2>&1
  ./rootcg.sh rootfix scx/rootfix/scx_lertick >> rootcg.txt 2>&1
done
log "rootcg done"
REPS=6 ./gate.sh rootfix scx/rootfix/scx_lertick > gate-rootfix.txt 2>&1
./cgp.sh scx/rootfix/scx_lertick > cg-rootfix.txt 2>&1
log "all done"
