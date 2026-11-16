#!/bin/bash
# Stall stress: hackbench with many groups on 4 CPUs under EEVDF, the current
# build (base) and the starvation guard (guard). Usage: stall-test.sh LABEL REPS
set -u
cd ~/lertick-exp
label=$1; reps=$2
OUT=stall/$label; mkdir -p $OUT
HB=rt-tests/hackbench; TS="taskset -c 0,1,2,3"
log() { echo "$(date +%T) $*" >> $OUT/progress.log; }
state() { cat /sys/kernel/sched_ext/state; }
wait_state() { for _ in $(seq 1 150); do [ "$(state)" = "$1" ] && return 0; sleep 0.1; done; return 1; }
ARMS=${ARMS:-"default base=kick/base/scx_lertick guard=guard/guard/scx_lertick"}
for rep in $(seq 0 $((reps - 1))); do
  for arm in $(printf '%s\n' $ARMS | shuf --random-source=<(yes "$label$rep")); do
    name=${arm%%=*}; d=$OUT/$rep-$name; mkdir -p $d; pid=
    [ "$(state)" = disabled ] || { log "state not disabled before $rep-$name, stop"; exit 1; }
    if [ "$arm" != default ]; then
      sudo ${arm#*=} --mode online --track-all --duration 0 > $d/sched.log 2>&1 &
      pid=$!
      wait_state enabled || { log "FAIL attach $rep-$name"; sudo kill -INT $pid; wait $pid; exit 1; }
    fi
    for g in 16 32; do $TS $HB -g $g -l 1000 > $d/hb-g$g.txt 2>&1; done
    if [ -n "$pid" ]; then
      [ "$(state)" = enabled ] || { log "FALLBACK $rep-$name"; touch $d/fallback; }
      sudo kill -INT $pid 2>/dev/null; wait $pid
      wait_state disabled || { log "FAIL detach $rep-$name"; exit 1; }
    fi
    log "ok $rep-$name"
  done
done
log "all done"
