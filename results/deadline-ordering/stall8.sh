#!/bin/bash
# Stall stress on 8 CPUs for scx builds: hackbench -g 16 and -g 32 (processes),
# Usage: ARMS="name=binary ..." stall8.sh LABEL REPS. Builds with a 1 s watchdog make the tail visible.
set -u
cd ~/lertick-exp
label=$1; reps=$2
OUT=stall/$label; mkdir -p $OUT
HB=rt-tests/hackbench
log() { echo "$(date +%T) $*" >> $OUT/progress.log; }
state() { cat /sys/kernel/sched_ext/state; }
wait_state() { for _ in $(seq 1 150); do [ "$(state)" = "$1" ] && return 0; sleep 0.1; done; return 1; }
for rep in $(seq 0 $((reps - 1))); do
  for arm in $(printf '%s\n' $ARMS | shuf --random-source=<(yes "$label$rep")); do
    name=${arm%%=*}; d=$OUT/$rep-$name; mkdir -p $d
    [ "$(state)" = disabled ] || { log "state not disabled before $rep-$name, stop"; exit 1; }
    sudo ${arm#*=} > $d/sched.log 2>&1 &
    pid=$!
    wait_state enabled || { log "FAIL attach $rep-$name"; sudo kill -INT $pid; wait $pid; exit 1; }
    for g in 16 32 16; do $HB -g $g -l 1000 >> $d/hb.txt 2>&1; done
    [ "$(state)" = enabled ] || { log "FALLBACK $rep-$name"; touch $d/fallback; }
    sudo kill -INT $pid 2>/dev/null; wait $pid
    wait_state disabled || { log "FAIL detach $rep-$name"; exit 1; }
    log "ok $rep-$name"
  done
done
log "all done"
