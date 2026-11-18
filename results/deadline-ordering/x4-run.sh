#!/bin/bash
# Deadline ordering (DEADLINE_SCALE 50, d50) against v2 and EEVDF, 8 runs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> x4-run.log; }
A="default v2=scx/v2/scx_lertick d50=scx/d50/scx_lertick,--min-train-samples,2"
./heldout.sh x4-cpu4 0,1,2,3 8 $A && log "cpu4 done" || log FAIL
./heldout.sh x4-cpu8 0,1,2,3,4,5,6,7 8 $A && log "all done" || log FAIL
