#!/bin/bash
# Confirm that the shared-policy build with one queue per LLC (one domain on
# this VM) matches the current build: 5 repetitions on 4 and 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> pol-run.log; }
O=",--mode,online,--track-all,--duration,0"
A="default base=aikeep/base/scx_lertick$O pol=llc/pol/scx_lertick$O"
./heldout.sh pol-cpu4 0,1,2,3 5 $A && log "cpu4 done" &&
./heldout.sh pol-cpu8 0,1,2,3,4,5,6,7 5 $A && log "all done" || log "FAIL run"
