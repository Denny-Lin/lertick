#!/bin/bash
# Wait for the lavd comparison, build scx_p2dq and scx_flash 1.1.4 with LLVM 20,
# smoke-test both, then run the same four comparison sets.
set -u
cd ~/lertick-exp
while kill -0 1003785 2>/dev/null; do sleep 20; done
log() { echo "$(date +%T) $*" >> more-run.log; }
log "lavd run finished; building"
cd ~/reference/scx-v1.1.4
source ~/.cargo/env 2>/dev/null
export PATH=$HOME/reference/llvm-20/bin:$PATH
cargo build --release -p scx_p2dq -p scx_flash > ~/reference/p2dq-flash-build.log 2>&1 || { log "FAIL build"; exit 1; }
cd ~/lertick-exp
for s in p2dq flash; do
  b=/home/ubuntu/reference/scx-v1.1.4/target/release/scx_$s
  sudo timeout -s INT 10 $b > ~/reference/$s-smoke.log 2>&1 &
  sleep 5; st=$(cat /sys/kernel/sched_ext/state); wait
  [ "$st" = enabled ] || { log "FAIL smoke $s ($st)"; exit 1; }
  log "smoke $s ok"
done
A="default rel-on=cur/release/scx_lertick,--mode,online,--track-all,--duration,0 p2dq=/home/ubuntu/reference/scx-v1.1.4/target/release/scx_p2dq flash=/home/ubuntu/reference/scx-v1.1.4/target/release/scx_flash"
./heldout.sh cpu4-more 0,1,2,3 5 $A && log "heldout cpu4 done" &&
./heldout.sh cpu8-more 0,1,2,3,4,5,6,7 5 $A && log "heldout cpu8 done" &&
./kbuild-long.sh cpu4-more 0,1,2,3 5 $A && log "kbuild cpu4 done" &&
./kbuild-long.sh cpu8-more 0,1,2,3,4,5,6,7 5 $A && log "all done" || log "FAIL run"
