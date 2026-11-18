#!/bin/bash
# Learned against fixed slices on the current build (fc): held-out on 4 and 8
# CPUs, 5 repetitions, then the kernel compile, 3 repetitions.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> ab-run.log; }
A="default online=scx/fc/scx_lertick fixed=scx/fc/scx_lertick,--fixed"
./heldout.sh ab-cpu4 0,1,2,3 5 $A && log "cpu4 done" &&
./heldout.sh ab-cpu8 0,1,2,3,4,5,6,7 5 $A && log "cpu8 done" &&
./kbuild-long.sh ab-cpu4 0,1,2,3 3 $A && ./kbuild-long.sh ab-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL"
