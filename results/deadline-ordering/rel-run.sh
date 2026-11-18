#!/bin/bash
# Benchmarks for the release build (all stability fixes, "lag") against EEVDF and
# the last measured default (b8): two sessions, held-out and kernel compile on 4 and 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> rel-run.log; }
A="default b8=scx/b8/scx_lertick lag=scx/lag/scx_lertick"
for s in rel1:5 rel2:3; do
  n=${s%%:*}; r=${s#*:}
  ./heldout.sh $n-cpu4 0,1,2,3 $r $A && ./heldout.sh $n-cpu8 0,1,2,3,4,5,6,7 $r $A &&
  ./kbuild-long.sh $n-cpu4 0,1,2,3 $r $A && ./kbuild-long.sh $n-cpu8 0,1,2,3,4,5,6,7 $r $A && log "$n done" || { log "FAIL $n"; exit 1; }
done
log "all done"
