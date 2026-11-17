#!/bin/bash
# Screen the migration and overrun candidates (scx builds) on 4 and 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> cand-run.log; }
A="default base=scx/b0/scx_lertick A=scx/A/scx_lertick C=scx/C/scx_lertick AC=scx/AC/scx_lertick ABC=scx/ABC/scx_lertick"
./heldout.sh cand-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh cand-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL run"
