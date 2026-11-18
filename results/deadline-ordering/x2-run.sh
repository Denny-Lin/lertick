#!/bin/bash
# 4-CPU wakeup p99: wakeup preemption of long-predicted runners (w1 any wakee,
# w2 short wakees, w1r5 after 0.5 ms) and a kick of the longest long runner (k2).
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> x2-run.log; }
A="default v2=scx/v2/scx_lertick w1=scx/w1/scx_lertick w2=scx/w2/scx_lertick w1r5=scx/w1r5/scx_lertick k2=scx/k2/scx_lertick"
./heldout.sh x2-cpu4 0,1,2,3 8 $A && log "cpu4 done" || log FAIL
