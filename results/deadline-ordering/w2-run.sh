#!/bin/bash
# Confirm W2 (1 ms slice past the prediction) against b1, 5 repetitions.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> w2-run.log; }
A="default b1=scx/b1/scx_lertick W2=scx/W2/scx_lertick"
./heldout.sh w2-cpu4 0,1,2,3 5 $A && log "heldout cpu4 done" &&
./heldout.sh w2-cpu8 0,1,2,3,4,5,6,7 5 $A && log "heldout cpu8 done" &&
./kbuild-long.sh w2-cpu4 0,1,2,3 5 $A && log "kbuild cpu4 done" &&
./kbuild-long.sh w2-cpu8 0,1,2,3,4,5,6,7 5 $A && log "all done" || log "FAIL run"
