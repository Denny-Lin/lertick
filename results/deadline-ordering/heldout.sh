#!/bin/bash
# Held-out tests. Usage: heldout.sh LABEL CPUS REPS ARM...
# ARM is "default" or NAME=COMMAND (a scheduler command line, commas for spaces).
set -u
cd ~/lertick-exp
label=$1; cpus=$2; reps=$3; shift 3
OUT=heldout/$label; mkdir -p $OUT
n=$(( $(echo $cpus | tr -cd , | wc -c) + 1 ))
SB=schbench/schbench; HB=rt-tests/hackbench; TS="taskset -c $cpus"
log() { echo "$(date +%T) $*" >> $OUT/progress.log; }
state() { cat /sys/kernel/sched_ext/state; }
wait_state() { for _ in $(seq 1 150); do [ "$(state)" = "$1" ] && return 0; sleep 0.1; done; return 1; }
kbuild() { # rebuild kernel/sched objects in a private build tree
  find kbuild/kernel/sched -name '*.o' -delete 2>/dev/null
  local s=$(date +%s.%N)
  $TS make -s -C ~/reference/linux-7.0 O=$HOME/lertick-exp/kbuild -j$n kernel/sched/built-in.a > /dev/null 2>&1
  echo "elapsed $(echo "$(date +%s.%N) - $s" | bc)"
}
for rep in $(seq 0 $((reps - 1))); do
  for arm in $(printf '%s\n' "$@" | shuf --random-source=<(yes "$label$rep")); do
    name=${arm%%=*}; d=$OUT/$rep-$name
    [ -f $d/done ] && continue
    mkdir -p $d; pid=
    [ "$(state)" = disabled ] || { log "state not disabled before $rep-$name, stop"; exit 1; }
    if [ "$arm" != default ]; then
      cmd=${arm#*=}; sudo ${cmd//,/ } > $d/sched.log 2>&1 &
      pid=$!
      wait_state enabled || { log "FAIL attach $rep-$name"; sudo kill -INT $pid; wait $pid; exit 1; }
    fi
    $TS perf bench sched messaging -g 10 -l 400 > $d/msg-proc.txt 2>&1
    $TS perf bench sched messaging -t -g 10 -l 400 > $d/msg-thread.txt 2>&1
    $TS perf bench sched pipe -l 200000 > $d/pipe.txt 2>&1
    $TS $SB -m 4 -t 2 -r 20 -w 3 > $d/sb-m4t2.txt 2>&1
    $TS $SB -m 1 -t 1 -r 20 -w 3 > $d/sb-m1t1.txt 2>&1
    $TS $HB -T -g 4 -l 2000 > $d/hb-thread.txt 2>&1
    $TS $HB -g 16 -l 1000 > $d/hb-g16.txt 2>&1
    kbuild > $d/kbuild.txt 2>&1
    if [ -n "$pid" ]; then
      [ "$(state)" = enabled ] || { log "FALLBACK $rep-$name"; touch $d/fallback; }
      sudo kill -INT $pid 2>/dev/null; wait $pid
      wait_state disabled || { log "FAIL detach $rep-$name"; exit 1; }
    fi
    touch $d/done; log "ok $rep-$name"
  done
done
log "all done"
