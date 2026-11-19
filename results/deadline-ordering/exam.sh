#!/bin/bash
# Held-out exam: EEVDF against the release, shuffled per repetition.
# Usage: exam.sh REPS ARM...   (ARM: default or NAME=COMMAND, commas for spaces)
cd ~/lertick-exp
reps=$1; shift
OUT=exam; mkdir -p $OUT
st() { cat /sys/kernel/sched_ext/state; }
log() { echo "$(date +%T) $*" >> $OUT/progress.log; }
ops() { grep -m1 "bogo-ops-per-second-real-time:" $1 | awk '{print $2}'; }
for rep in $(seq $reps); do
  for arm in $(printf '%s\n' "$@" | shuf); do
    name=${arm%%=*}; d=$OUT/$rep-$name; mkdir -p $d; pid=
    if [ "$arm" != default ]; then cmd=${arm#*=}; sudo ${cmd//,/ } > $d/sched.log 2>&1 & pid=$!
      for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done; fi
    # game beside 4 CPU hogs on CPUs 0-3
    taskset -c 0-3 stress-ng --cpu 4 --timeout 22s > /dev/null 2>&1 & hog=$!
    sleep 1; taskset -c 0-3 ./gamesim 60 4 6 5 0.5 20 > $d/game.txt 2>&1; wait $hog
    for s in futex mq matrix fork; do
      taskset -c 0-7 stress-ng --$s 8 --timeout 15s --metrics-brief --yaml $d/$s.y > /dev/null 2>&1
      echo "$s $(ops $d/$s.y)" >> $d/stress.txt
    done
    t0=$(date +%s.%N); tar cf - -C ~/reference/linux-7.0 drivers/net | taskset -c 0-7 zstd -T8 -10 -q -o /dev/null; t1=$(date +%s.%N)
    echo "zstd $(echo "$t1 - $t0" | bc)" > $d/zstd.txt
    find kbuild/mm -name '*.o' -delete 2>/dev/null
    t0=$(date +%s.%N); taskset -c 0-7 make -s -C ~/reference/linux-7.0 O=$HOME/lertick-exp/kbuild -j8 mm/ > $d/mm.log 2>&1; rc=$?; t1=$(date +%s.%N)
    echo "mm $(echo "$t1 - $t0" | bc) rc=$rc" > $d/mm.txt
    if [ -n "$pid" ]; then [ "$(st)" = enabled ] || { log "FALLBACK $rep-$name"; touch $d/fallback; }; sudo kill -INT $pid; wait $pid; fi
    for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
    touch $d/done; log "ok $rep-$name"
  done
done
log "all done"
