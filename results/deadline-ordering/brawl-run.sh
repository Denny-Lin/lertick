#!/bin/bash
# Same-session comparison: EEVDF, the release candidate and four sched_ext
# schedulers, shuffled per repetition, 8 runs on 4 and 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> brawl-run.log; }
R=$HOME/reference
A="default lertick=scx/v2/scx_lertick bpfland=$R/scx-v1.1.2/target/release/scx_bpfland lavd=$R/scx-v1.1.4/target/release/scx_lavd p2dq=$R/scx-v1.1.4/target/release/scx_p2dq flash=$R/scx-v1.1.4/target/release/scx_flash"
./heldout.sh brawl-cpu4 0,1,2,3 8 $A && log "cpu4 done" || log FAIL
./heldout.sh brawl-cpu8 0,1,2,3,4,5,6,7 8 $A && log "all done" || log FAIL
