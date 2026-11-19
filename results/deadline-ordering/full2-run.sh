#!/bin/bash
# Full held-out set: EEVDF, h48, pcm (learned completion time), 6 reps, 8 then 4 CPUs.
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> full2-run.log; }
A="default"; for a in h48 pcm; do A="$A $a=scx/$a/scx_lertick,--min-train-samples,2"; done
log start
./heldout.sh full2-cpu8 0,1,2,3,4,5,6,7 6 $A && log "cpu8 done" || log "cpu8 FAIL"
./heldout.sh full2-cpu4 0,1,2,3 6 $A && log "cpu4 done" || log "cpu4 FAIL"
log "all done"
