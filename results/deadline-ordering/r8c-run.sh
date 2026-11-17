#!/bin/bash
# Confirm the shorter slice bounds against b7, then stress and gate both.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> r8c-run.log; }
A="default b7=scx/b7/scx_lertick M3=scx/b7/scx_lertick,-M,3000 M3s1=scx/b7/scx_lertick,-M,3000,-s,1000"
./heldout.sh r8c-cpu4 0,1,2,3 5 $A && log "heldout cpu4 done" &&
./heldout.sh r8c-cpu8 0,1,2,3,4,5,6,7 5 $A && log "heldout cpu8 done" &&
./kbuild-long.sh r8c-cpu4 0,1,2,3 5 $A && log "kbuild cpu4 done" &&
./kbuild-long.sh r8c-cpu8 0,1,2,3,4,5,6,7 5 $A && log "kbuild done" &&
./stress-run.sh m3s1 scx/b7/scx_lertick -M 3000 -s 1000 > /dev/null 2>&1 &&
./stress-run.sh m3 scx/b7/scx_lertick -M 3000 > /dev/null 2>&1 && log "stress done" &&
REPS=6 ./gate.sh M3s1 scx/b7/scx_lertick -M 3000 -s 1000 > gate-r8c.txt 2>&1 &&
REPS=6 ./gate.sh M3 scx/b7/scx_lertick -M 3000 >> gate-r8c.txt 2>&1 && log "all done" || log "FAIL run"
