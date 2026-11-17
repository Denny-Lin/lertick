#!/bin/bash
# Screen the migration and overrun candidates (scx builds) on 4 and 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> r8-run.log; }
A="default b7=scx/b7/scx_lertick W16=scx/W16/scx_lertick M3=scx/b7/scx_lertick,-M,3000 M3s1=scx/b7/scx_lertick,-M,3000,-s,1000"
./heldout.sh r8-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh r8-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL run"
