#!/bin/bash
# Baseline of the scx submission build against EEVDF, with the product build
# in the same sessions: held-out and kernel compile on 4 and 8 CPUs, 5 reps.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> scx-base-run.log; }
A="default scx=scx/b0/scx_lertick prod=scx/prod/scx_lertick,--mode,online,--track-all,--duration,0"
( while sleep 10; do echo "$(date +%s) $(head -1 /proc/stat)"; done ) > scx-base-steal.log &
stealpid=$!
./heldout.sh scxb-cpu4 0,1,2,3 5 $A && log "heldout cpu4 done" &&
./heldout.sh scxb-cpu8 0,1,2,3,4,5,6,7 5 $A && log "heldout cpu8 done" &&
./kbuild-long.sh scxb-cpu4 0,1,2,3 5 $A && log "kbuild cpu4 done" &&
./kbuild-long.sh scxb-cpu8 0,1,2,3,4,5,6,7 5 $A && log "all done" || log "FAIL run"
kill $stealpid
