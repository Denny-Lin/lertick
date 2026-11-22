#!/bin/bash
# scale.sh REPS ARM...: futex ops/s and perf messaging time on 1, 2, 4, 6 and 8 CPUs.
cd ~/lertick-exp
st() { cat /sys/kernel/sched_ext/state; }
ops() { grep -m1 "bogo-ops-per-second-real-time:" $1 | awk '{printf "%d", $2}'; }
reps=$1; shift
for r in $(seq $reps); do for arm in $(printf '%s\n' "$@" | shuf); do name=${arm%%=*}; pid=
  if [ "$arm" != default ]; then cmd=${arm#*=}; sudo ${cmd//,/ } > /dev/null 2>&1 & pid=$!
    for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
    [ "$(st)" = enabled ] || { echo "$name ATTACH-FAIL"; continue; }; fi
  line="$name"
  for n in 1 2 4 6 8; do cpus=$(seq -s, 0 $((n-1)))
    taskset -c $cpus stress-ng --futex $n -t 4 --metrics-brief --yaml /tmp/sc.y >/dev/null 2>&1
    m=$(taskset -c $cpus perf bench sched messaging -g 10 -l 300 2>&1 | awk '/Total time/{print $3}')
    line="$line f$n=$(ops /tmp/sc.y) m$n=$m"
  done
  echo "$line state=$(st)"
  [ -n "$pid" ] && { sudo kill -INT $pid; wait $pid; }
  for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
done; done
