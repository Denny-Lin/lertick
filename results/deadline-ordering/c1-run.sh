#!/bin/bash
# Screen the migration and overrun candidates (scx builds) on 4 and 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> c1-run.log; }
A="default base=scx/b0/scx_lertick C1=scx/C1/scx_lertick"
./heldout.sh c1-cpu4 0,1,2,3 5 $A && log "cpu4 done" &&
./heldout.sh c1-cpu8 0,1,2,3,4,5,6,7 5 $A && log "all done" || log "FAIL run"
