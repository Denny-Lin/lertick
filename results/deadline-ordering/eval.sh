#!/bin/bash
# Screening on community benchmarks. Usage: eval.sh LABEL REPS ARM...
# ARM is "default" (EEVDF) or NAME=BUILD_DIR:MODE[:EXTRA], with BUILD_DIR under ~/lertick-exp/cur
# and EXTRA extra loader options with commas for spaces.
# Arm order is shuffled per repetition; each arm runs every configuration under one attach.
set -u
cd ~/lertick-exp
label=$1; reps=$2; shift 2
OUT=eval/$label; mkdir -p $OUT
SB=schbench/schbench; HB=rt-tests/hackbench
SBTIME=${SBTIME:-15}; SBWARM=${SBWARM:-3}
log() { echo "$(date +%T) $*" >> $OUT/progress.log; }
state() { cat /sys/kernel/sched_ext/state; }
wait_state() { for _ in $(seq 1 100); do [ "$(state)" = "$1" ] && return 0; sleep 0.1; done; return 1; }
for rep in $(seq 0 $((reps - 1))); do
  for arm in $(printf '%s\n' "$@" | shuf --random-source=<(yes "$label$rep")); do
    name=${arm%%=*}; d=$OUT/$rep-$name
    [ -f $d/done ] && continue
    mkdir -p $d; pid=
    [ "$(state)" = disabled ] || { log "state not disabled before $rep-$name, stop"; exit 1; }
    if [ "$arm" != default ]; then
      IFS=: read -r build mode extra <<< "${arm#*=}"
      sudo cur/$build/scx_lertick --mode $mode --track-all --duration 0 ${extra//,/ } > $d/loader.log 2>&1 &
      pid=$!
      wait_state enabled || { log "FAIL attach $rep-$name"; sudo kill -INT $pid; wait $pid; exit 1; }
    fi
    for c in "light:-m 1 -t 2" "full:-m 1 -t 4" "overload:-m 2 -t 6"; do
      taskset -c 0-3 $SB ${c#*:} -r $SBTIME -w $SBWARM > $d/schbench-${c%%:*}.txt 2>&1
    done
    for c in "socket:-g 8 -l 2000" "pipe:-p -g 8 -l 2000"; do
      taskset -c 0-3 $HB ${c#*:} > $d/hackbench-${c%%:*}.txt 2>&1
    done
    if [ -n "$pid" ]; then
      [ "$(state)" = enabled ] || { log "FALLBACK $rep-$name"; touch $d/fallback; }
      sudo kill -INT $pid 2>/dev/null; wait $pid
      wait_state disabled || { log "FAIL detach $rep-$name"; exit 1; }
    fi
    touch $d/done; log "ok $rep-$name"
  done
done
log "all done"
