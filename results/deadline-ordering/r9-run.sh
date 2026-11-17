#!/bin/bash
# Round 9: gate, then screen the slice-bound candidates around b8 on 4 and 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> r9-run.log; }
B=scx/b8/scx_lertick
for c in "M2 -M 2000" "M25 -M 2500" "s15 -s 1500" "M2s15 -M 2000 -s 1500"; do
  set -- $c; n=$1; shift
  REPS=3 ./gate.sh $n $B "$@" >> gate-r9.txt 2>&1
done
log "gate done"
A="default b8=$B M2=$B,-M,2000 M25=$B,-M,2500 s15=$B,-s,1500 M2s15=$B,-M,2000,-s,1500"
./heldout.sh r9-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh r9-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL run"
