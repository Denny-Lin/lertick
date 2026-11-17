#!/bin/bash
# Screen the migration and overrun candidates (scx builds) on 4 and 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> cand3-run.log; }
A="default base=scx/b0/scx_lertick C1=scx/C1/scx_lertick C1K=scx/C1K/scx_lertick CK=scx/CK/scx_lertick"
./heldout.sh cand3-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh cand3-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL run"
