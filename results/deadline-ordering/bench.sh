#!/bin/bash
# schbench and hackbench: EEVDF vs Lertick fixed and online, 10 repetitions,
# arm order shuffled per repetition. Run as the ubuntu user from ~/lertick-exp.
set -u
cd ~/lertick-exp
OUT=bench; mkdir -p $OUT
LOADER=base/build/scx_lertick
SCHBENCH=schbench/schbench
HACKBENCH=rt-tests/hackbench
log() { echo "$(date +%T) $*" >> $OUT/progress.log; }
state() { cat /sys/kernel/sched_ext/state; }
wait_state() { for _ in $(seq 1 100); do [ "$(state)" = "$1" ] && return 0; sleep 0.1; done; return 1; }

run_configs() { # arm rep
  local d=$OUT/$2-$1
  mkdir -p $d
  for c in "light:-m 1 -t 2" "full:-m 1 -t 4" "overload:-m 2 -t 6"; do
    taskset -c 0-3 $SCHBENCH ${c#*:} -r 30 -w 5 > $d/schbench-${c%%:*}.txt 2>&1
  done
  for c in "socket:-g 8 -l 2000" "pipe:-p -g 8 -l 2000"; do
    taskset -c 0-3 $HACKBENCH ${c#*:} > $d/hackbench-${c%%:*}.txt 2>&1
  done
}

for rep in $(seq 0 9); do
  for arm in $(printf 'default\nfixed\nonline\n' | shuf --random-source=<(yes $((20261120 + rep)))); do
    [ -f $OUT/$rep-$arm/done ] && continue
    [ "$(state)" = disabled ] || { log "state not disabled before $rep-$arm, stop"; exit 1; }
    pid=
    if [ $arm != default ]; then
      sudo $LOADER --mode $arm --track-all --duration 0 > $OUT/$rep-$arm.loader.log 2>&1 &
      pid=$!
      wait_state enabled || { log "FAIL attach $rep-$arm"; sudo kill -INT $pid; wait $pid; exit 1; }
    fi
    run_configs $arm $rep
    if [ -n "$pid" ]; then
      [ "$(state)" = enabled ] || log "WARN $rep-$arm fell back during the run"
      sudo kill -INT $pid; wait $pid
      wait_state disabled || { log "FAIL detach $rep-$arm"; exit 1; }
    fi
    touch $OUT/$rep-$arm/done; log "ok $rep-$arm"
  done
done
log "all done"
