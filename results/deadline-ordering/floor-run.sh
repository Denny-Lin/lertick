#!/bin/bash
# Learned slices never shorter than the fixed slice (floor): online with
# --slice-min-us 2000 against online, fixed and EEVDF, 4 and 8 CPUs, 5 repetitions.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> floor-run.log; }
until grep -qE "^[0-9:]+ (all done|FAIL)" ab-run.log 2>/dev/null; do sleep 20; done
B=scx/fc/scx_lertick
REPS=3 ./gate.sh floor $B -L 2000 > gate-floor.txt 2>&1
grep -q "nice-mix passed 3/3" gate-floor.txt || { log "FAIL gate"; exit 1; }
log "gate done"
A="default fixed=$B,--fixed online=$B floor=$B,-L,2000"
./heldout.sh floor-cpu4 0,1,2,3 5 $A && log "cpu4 done" &&
./heldout.sh floor-cpu8 0,1,2,3,4,5,6,7 5 $A && log "all done" || log "FAIL run"
