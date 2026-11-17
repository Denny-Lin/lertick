#!/bin/bash
# Comparison with an idle host: every scheduler in the same sessions. Records
# guest steal time every 10 s. Usage: night-run.sh [ROUNDS] [PREFIX] [DELAY_S]
set -u
rounds=${1:-2}; prefix=${2:-night}; delay=${3:-0}
cd ~/lertick-exp
sleep "$delay"
log() { echo "$(date +%T) $*" >> $prefix-run.log; }
R=/home/ubuntu/reference/scx-v1.1.4/target/release
A="default t5=t5/build/scx_lertick,--mode,online,--track-all,--duration,0"
A="$A bpfland=/home/ubuntu/reference/scx-v1.1.2/target/release/scx_bpfland"
A="$A lavd=$R/scx_lavd p2dq=$R/scx_p2dq flash=$R/scx_flash"
( while sleep 10; do echo "$(date +%s) $(head -1 /proc/stat)"; done ) > $prefix-steal.log &
stealpid=$!
for round in $(seq 1 "$rounds"); do
  ./heldout.sh $prefix$round-cpu4 0,1,2,3 5 $A && log "round $round heldout cpu4 done" &&
  ./heldout.sh $prefix$round-cpu8 0,1,2,3,4,5,6,7 5 $A && log "round $round heldout cpu8 done" &&
  ./kbuild-long.sh $prefix$round-cpu4 0,1,2,3 5 $A && log "round $round kbuild cpu4 done" &&
  ./kbuild-long.sh $prefix$round-cpu8 0,1,2,3,4,5,6,7 5 $A && log "round $round done" || { log "FAIL round $round"; break; }
done
kill $stealpid
log "all done"
