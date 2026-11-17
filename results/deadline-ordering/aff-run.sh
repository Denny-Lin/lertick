#!/bin/bash
# Screen the migration and overrun candidates (scx builds) on 4 and 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> aff-run.log; }
A="default b0=scx/b0/scx_lertick D16W4=scx/D16W4/scx_lertick D32W4=scx/D32W4/scx_lertick D16W8=scx/D16W8/scx_lertick"
./heldout.sh aff-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh aff-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL run"
