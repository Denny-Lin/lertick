#!/bin/bash
# Full held-out set: EEVDF, sp3 (release), wk3 (EEVDF-style wakeup preemption), 6 reps, 8 then 4 CPUs.
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> wk3full-run.log; }
A="default"; for a in sp3 wk3; do A="$A $a=scx/$a/scx_lertick,--min-train-samples,2"; done
log start
./heldout.sh wk3full-cpu8 0,1,2,3,4,5,6,7 6 $A && log "cpu8 done" || log "cpu8 FAIL"
./heldout.sh wk3full-cpu4 0,1,2,3 6 $A && log "cpu4 done" || log "cpu4 FAIL"
log "all done"
