#!/bin/bash
# Screen the migration and overrun candidates (scx builds) on 4 and 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> r5-run.log; }
A="default b2=scx/b2/scx_lertick T05=scx/T05/scx_lertick T2=scx/T2/scx_lertick"
./heldout.sh r5-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh r5-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL run"
