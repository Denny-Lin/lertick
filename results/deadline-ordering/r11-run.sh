#!/bin/bash
# Round 11: fixed slice on 4 CPUs (screen for a CPU-count scaled fixed slice).
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> r11-run.log; }
until grep -q "all done\|FAIL" r10-run.log 2>/dev/null; do sleep 30; done
B=scx/b8/scx_lertick
for c in "s125 -s 1250"; do set -- $c; n=$1; shift; REPS=3 ./gate.sh $n $B "$@" >> gate-r11.txt 2>&1; done
log "gate done"
A="default b8=$B s1=$B,-s,1000 s125=$B,-s,1250 s15=$B,-s,1500"
./heldout.sh r11-cpu4 0,1,2,3 5 $A && log "all done" || log "FAIL run"
