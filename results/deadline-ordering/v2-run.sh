#!/bin/bash
# The V2 model in use, with and without the program table: build, gate, check
# that the table is hit during a kernel build, then held-out on 4 and 8 CPUs
# (3 repetitions) and kbuild-long (3), against EEVDF, fixed and the old model.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> v2-run.log; }
for v in v2 v2tab; do cp /tmp/port-$v.tgz /tmp/port.tgz; /tmp/scx-build.sh $v > /tmp/$v.build.log 2>&1 || { log "FAIL build $v"; exit 1; }; done
for v in v2 v2tab; do REPS=3 ./gate.sh $v scx/$v/scx_lertick >> gate-v2.txt 2>&1; done
[ "$(grep -c 'nice-mix passed 3/3' gate-v2.txt)" = 2 ] || { log "FAIL gate"; exit 1; }
sudo scx/v2tab/scx_lertick > /tmp/v2tab-hits.log 2>&1 & pid=$!
for _ in $(seq 100); do [ "$(cat /sys/kernel/sched_ext/state)" = enabled ] && break; sleep 0.1; done
find kbuild/kernel -name '*.o' -delete; taskset -c 0-3 make -s -C ~/reference/linux-7.0 O=$HOME/lertick-exp/kbuild -j8 kernel/ > /dev/null 2>&1
id=$(sudo bpftool -j map show | python3 -c 'import json,sys; print([m["id"] for m in json.load(sys.stdin) if m.get("name","").endswith(".bss")][-1])')
log "table hits during a kernel build: $(sudo bpftool map dump id $id | grep -A0 nr_prog_hits | tr -dc 0-9)"
sudo kill -INT $pid; wait $pid
log "gate done"
B=scx/fc/scx_lertick
A="default fixed=$B,--fixed online=$B v2=scx/v2/scx_lertick v2tab=scx/v2tab/scx_lertick"
./heldout.sh v2-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh v2-cpu8 0,1,2,3,4,5,6,7 3 $A && log "cpu8 done" &&
./kbuild-long.sh v2-cpu4 0,1,2,3 3 $A && ./kbuild-long.sh v2-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL run"
