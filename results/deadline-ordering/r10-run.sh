#!/bin/bash
# Round 10: build, gate, then screen policy-constant candidates around b8.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> r10-run.log; }
/tmp/scx-build.sh SW1 SYNC_WINDOW_NS=1000000ULL && /tmp/scx-build.sh SW0 SYNC_WINDOW_NS=0ULL &&
/tmp/scx-build.sh H8 HEAD_SCAN=8 && /tmp/scx-build.sh D32 AFFINE_DEPTH=32 || { log "FAIL build"; exit 1; }
for n in SW1 SW0 H8 D32; do REPS=3 ./gate.sh $n scx/$n/scx_lertick >> gate-r10.txt 2>&1; done
log "gate done"
A="default b8=scx/b8/scx_lertick SW1=scx/SW1/scx_lertick SW0=scx/SW0/scx_lertick H8=scx/H8/scx_lertick D32=scx/D32/scx_lertick"
./heldout.sh r10-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh r10-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL run"
