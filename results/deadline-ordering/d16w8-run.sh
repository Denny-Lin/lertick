#!/bin/bash
# Confirm D16W8 (affine pick over 16 tasks within 8 ms) against b0.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> d16w8-run.log; }
A="default b0=scx/b0/scx_lertick D16W8=scx/D16W8/scx_lertick"
./heldout.sh d16w8-cpu4 0,1,2,3 5 $A && log "heldout cpu4 done" &&
./heldout.sh d16w8-cpu8 0,1,2,3,4,5,6,7 5 $A && log "heldout cpu8 done" &&
./kbuild-long.sh d16w8-cpu4 0,1,2,3 5 $A && log "kbuild cpu4 done" &&
./kbuild-long.sh d16w8-cpu8 0,1,2,3,4,5,6,7 5 $A && log "kbuild done" &&
./stress-run.sh d16w8 scx/D16W8/scx_lertick > /dev/null 2>&1 && log "stress done" &&
REPS=3 ./gate.sh D16W8 scx/D16W8/scx_lertick > gate-d16w8.txt 2>&1 && log "all done" || log "FAIL run"
