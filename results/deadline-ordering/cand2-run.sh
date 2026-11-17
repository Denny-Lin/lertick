#!/bin/bash
# Screen the migration and overrun candidates (scx builds) on 4 and 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> cand2-run.log; }
A="default base=scx/b0/scx_lertick AC2=scx/AC2/scx_lertick ACK=scx/ACK/scx_lertick CK=scx/CK/scx_lertick"
./heldout.sh cand2-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh cand2-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL run"
