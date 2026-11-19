#!/bin/bash
# Full held-out set: EEVDF, h0 (current), h48 (sync cap 48), 6 reps, 8 then 4 CPUs.
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> cap48-run.log; }
A="default"; for a in h0 h48; do A="$A $a=scx/$a/scx_lertick,--min-train-samples,2"; done
log start
./heldout.sh cap48-cpu8 0,1,2,3,4,5,6,7 6 $A && log "cpu8 done" || log "cpu8 FAIL"
./heldout.sh cap48-cpu4 0,1,2,3 6 $A && log "cpu4 done" || log "cpu4 FAIL"
log "all done"
