#!/bin/bash
# Longer kernel compile for the C1 confirmation, 4 and 8 CPUs, 5 reps.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> c1-kb-run.log; }
A="default base=scx/b0/scx_lertick C1=scx/C1/scx_lertick"
./kbuild-long.sh c1-cpu4 0,1,2,3 5 $A && log "cpu4 done" &&
./kbuild-long.sh c1-cpu8 0,1,2,3,4,5,6,7 5 $A && log "all done" || log "FAIL run"
