#!/bin/bash
# Screen the learned continuation (ek) after the evening round finishes.
set -u
cd ~/lertick-exp
while [ -n "$(pgrep -x -f "/bin/bash ./night-run.sh 1 eve [0-9]*")" ]; do sleep 30; done
log() { echo "$(date +%T) $*" >> ek-run.log; }
O=",--track-all,--duration,0"
A="default base=aikeep/base/scx_lertick,--mode,online$O fixed=aikeep/base/scx_lertick,--mode,fixed$O ek=aikeep/ek/scx_lertick,--mode,online$O"
./heldout.sh ek-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh ek-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL run"
