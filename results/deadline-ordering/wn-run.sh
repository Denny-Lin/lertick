#!/bin/bash
# Waker model (N) against the PR build, F and EEVDF, 4 and 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> wn-run.log; }
A="default pr=scx/prrel/scx_lertick f=scx/pf/scx_lertick,--min-train-samples,2 n=scx/pn/scx_lertick,--min-train-samples,2"
./heldout.sh wn-cpu4 0,1,2,3 5 $A && log "cpu4 done" &&
./heldout.sh wn-cpu8 0,1,2,3,4,5,6,7 5 $A && log "all done" || log "FAIL"
