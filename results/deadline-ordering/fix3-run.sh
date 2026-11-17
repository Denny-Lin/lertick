#!/bin/bash
# Atomic clock: held-out regression check against b8, then the stress battery.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> fix3-run.log; }
A="default b8=scx/b8/scx_lertick b9=scx/f-new/scx_lertick"
./heldout.sh fix-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh fix-cpu8 0,1,2,3,4,5,6,7 3 $A && log "heldout done" || { log "FAIL run"; exit 1; }
./stress-run.sh b9 scx/f-new/scx_lertick > /dev/null 2>&1 && log "stress done" || log "FAIL stress"
GROUPS_LIST="32 32 32" ARMS="b8=scx/b8/scx_lertick b9=scx/f-new/scx_lertick" ./stall8.sh atomic32 15
log "all done"
