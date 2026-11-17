#!/bin/bash
# Screen the migration and overrun candidates (scx builds) on 4 and 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> wake-run.log; }
A="default b1=scx/b1/scx_lertick W1=scx/W1/scx_lertick W2=scx/W2/scx_lertick W3=scx/W3/scx_lertick W4=scx/W4/scx_lertick"
./heldout.sh wake-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh wake-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL run"
