#!/bin/bash
# Screen one queue per LLC domain. The VM has one LLC, so `split` builds force
# two domains: groups of 2 CPUs for the 4-CPU run, groups of 4 for 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> llc-run.log; }
O=",--mode,online,--track-all,--duration,0"
A="default base=aikeep/base/scx_lertick$O one=llc/one/scx_lertick$O"
./heldout.sh llc-cpu4 0,1,2,3 3 $A split=llc/s2/scx_lertick$O && log "cpu4 done" &&
./heldout.sh llc-cpu8 0,1,2,3,4,5,6,7 3 $A split=llc/s4/scx_lertick$O && log "all done" || log "FAIL run"
