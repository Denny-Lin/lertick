#!/bin/bash
# fix-screen.sh REPS ARM...: the cases pe2c failed and futex: stress-ng mq on
# 8 CPUs, perf messaging on 1 CPU, futex on 8 and 1 CPUs.
cd ~/lertick-exp
st() { cat /sys/kernel/sched_ext/state; }
ops() { grep -m1 "bogo-ops-per-second-real-time:" $1 | awk '{printf "%d", $2}'; }
reps=$1; shift
for r in $(seq $reps); do for arm in $(printf '%s\n' "$@" | shuf); do name=${arm%%=*}; pid=
  if [ "$arm" != default ]; then cmd=${arm#*=}; sudo ${cmd//,/ } > /dev/null 2>&1 & pid=$!
    for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
    [ "$(st)" = enabled ] || { echo "$name ATTACH-FAIL"; continue; }; fi
  taskset -c 0-7 stress-ng --mq 8 -t 6 --metrics-brief --yaml /tmp/fq.y >/dev/null 2>&1; mq=$(ops /tmp/fq.y)
  m1=$(taskset -c 0 perf bench sched messaging -g 10 -l 200 2>&1 | awk '/Total time/{print $3}')
  stress-ng --futex 8 -t 5 --metrics-brief --yaml /tmp/ff.y >/dev/null 2>&1; f8=$(ops /tmp/ff.y)
  taskset -c 0 stress-ng --futex 1 -t 4 --metrics-brief --yaml /tmp/f1.y >/dev/null 2>&1; f1=$(ops /tmp/f1.y)
  echo "$name mq=$mq msg1=$m1 f8=$f8 f1=$f1 state=$(st)"
  [ -n "$pid" ] && { sudo kill -INT $pid; wait $pid; }
  for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
done; done
