#!/bin/bash
# F, N and both with LONG_TO_FIXED, 4 and 8 CPUs, 8 repetitions.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> wo-run.log; }
A="f=scx/pf/scx_lertick,--min-train-samples,2 n=scx/pn/scx_lertick,--min-train-samples,2 fo=scx/pfo/scx_lertick,--min-train-samples,2 no=scx/pno/scx_lertick,--min-train-samples,2"
./heldout.sh wo-cpu4 0,1,2,3 8 $A && log "cpu4 done" &&
./heldout.sh wo-cpu8 0,1,2,3,4,5,6,7 8 $A && log "all done" || log "FAIL"
