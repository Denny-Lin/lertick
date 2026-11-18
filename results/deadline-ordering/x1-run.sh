#!/bin/bash
# 4-CPU wakeup p99: release (v2) against a shorter slice for bursts predicted
# past slice_max (1 and 1.5 ms) and a preemption of such a burst by a short
# wakee (k1), 8 runs, then 8 CPUs for any candidate.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> x1-run.log; }
A="default v2=scx/v2/scx_lertick q10=scx/q10/scx_lertick,--min-train-samples,2 q15=scx/q15/scx_lertick,--min-train-samples,2 k1=scx/k1/scx_lertick,--min-train-samples,2"
./heldout.sh x1-cpu4 0,1,2,3 8 $A && log "cpu4 done" || log FAIL
./heldout.sh x1-cpu8 0,1,2,3,4,5,6,7 8 $A && log "all done" || log FAIL
