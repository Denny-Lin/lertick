#!/bin/bash
# Queue rules behind the 4-CPU wakeup p99: KEEP_MAX_NS 5 ms (km5), the affine
# pick's window 4 ms (aw4), and both, against v2 and EEVDF, 8 runs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> x3-run.log; }
T=",--min-train-samples,2"
A="default v2=scx/v2/scx_lertick km5=scx/km5/scx_lertick$T aw4=scx/aw4/scx_lertick$T km5aw4=scx/km5aw4/scx_lertick$T"
./heldout.sh x3-cpu4 0,1,2,3 8 $A && log "cpu4 done" || log FAIL
./heldout.sh x3-cpu8 0,1,2,3,4,5,6,7 8 $A && log "all done" || log FAIL
