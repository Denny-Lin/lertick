#!/bin/bash
# Slice experiments after ab-run.sh, on the current build (fc):
#   floor:  online with --slice-min-us 2000 (never shorter than the fixed slice)
#   finish: LERTICK_SLICE_FINISH, a burst predicted to end within the slice cap
#           gets the rest of it, every other task the fixed slice
# Gate both, then held-out on 4 and 8 CPUs, 5 repetitions, with EEVDF, fixed and online.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> floor-run.log; }
until grep -qE "^[0-9:]+ (all done|FAIL)" ab-run.log 2>/dev/null; do sleep 20; done
cp /tmp/port-fin.tgz /tmp/port.tgz; /tmp/scx-build.sh fin > /tmp/fin.build.log 2>&1 || { log "FAIL build"; tail -20 /tmp/fin.build.log >> floor-run.log; exit 1; }
B=scx/fc/scx_lertick
REPS=3 ./gate.sh floor $B -L 2000 > gate-floor.txt 2>&1
REPS=3 ./gate.sh finish scx/fin/scx_lertick >> gate-floor.txt 2>&1
[ "$(grep -c 'nice-mix passed 3/3' gate-floor.txt)" = 2 ] || { log "FAIL gate"; exit 1; }
log "gate done"
A="default fixed=$B,--fixed online=$B floor=$B,-L,2000 finish=scx/fin/scx_lertick"
./heldout.sh floor-cpu4 0,1,2,3 5 $A && log "cpu4 done" &&
./heldout.sh floor-cpu8 0,1,2,3,4,5,6,7 5 $A && log "all done" || log "FAIL run"
