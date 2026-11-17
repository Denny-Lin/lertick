#!/bin/bash
# Positive-lag clamp: build, gate, cgroup weights, stall stress, held-out screening against b9.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> cap-run.log; }
for v in cap cap-w1; do cp /tmp/port-$v.tgz /tmp/port.tgz; /tmp/scx-build.sh $v || { log "FAIL build $v"; exit 1; }; done
export PKG_CONFIG_PATH=/home/ubuntu/lertick-pr/build/deps/install/lib/pkgconfig
(cd llc && rm -rf cap-tree && mkdir cap-tree && cd cap-tree && tar xzf /tmp/pol-src.tgz 2>/dev/null && make -s build/scx_lertick > ../cap.build.log 2>&1 && make -s test-model test-accounting > ../cap.test.log 2>&1) || { log "FAIL product"; exit 1; }
log "built"
REPS=4 ./gate.sh cap scx/cap/scx_lertick >> gate-cap.txt 2>&1
REPS=2 ./gate.sh capprod llc/cap-tree/build/scx_lertick >> gate-cap.txt 2>&1
log "gate done"
for a in "eevdf" "b9 scx/f-new/scx_lertick" "cap scx/cap/scx_lertick"; do set -- $a; n=$1; shift; echo "== $n" >> cg-cap.txt; ./cgp.sh "$@" >> cg-cap.txt 2>&1; done
log "cgroup done"
ARMS="b9=scx/f-new-w1/scx_lertick cap=scx/cap-w1/scx_lertick" ./stall8.sh cap 12
log "stall done"
A="default b9=scx/f-new/scx_lertick cap=scx/cap/scx_lertick"
./heldout.sh cap-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh cap-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL run"
