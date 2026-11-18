#!/bin/bash
# Program memory (tab): finish plus LERTICK_PROG_TABLE. Build after the floor
# run, gate, then held-out on 4 and 8 CPUs, 3 repetitions, against finish and EEVDF.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> tab-run.log; }
until grep -qE "^[0-9:]+ (all done|FAIL)" floor-run.log 2>/dev/null; do sleep 20; done
cp /tmp/port-tab.tgz /tmp/port.tgz; /tmp/scx-build.sh tab > /tmp/tab.build.log 2>&1 || { log "FAIL build"; grep -iE "error|R[0-9]" /tmp/tab.build.log | tail -20 >> tab-run.log; exit 1; }
REPS=3 ./gate.sh tab scx/tab/scx_lertick > gate-tab.txt 2>&1
grep -q "nice-mix passed 3/3" gate-tab.txt || { log "FAIL gate"; exit 1; }
log "gate done"
A="default finish=scx/fin/scx_lertick tab=scx/tab/scx_lertick"
./heldout.sh tab-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh tab-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL run"
