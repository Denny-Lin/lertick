#!/bin/bash
# fx-cs.sh REPS ARM...: 8-CPU futex ops/s and context switches per second.
cd ~/lertick-exp
st() { cat /sys/kernel/sched_ext/state; }
reps=$1; shift
for r in $(seq $reps); do for arm in $(printf '%s\n' "$@" | shuf); do name=${arm%%=*}; pid=
  if [ "$arm" != default ]; then cmd=${arm#*=}; sudo ${cmd//,/ } > /dev/null 2>&1 & pid=$!
    for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
    [ "$(st)" = enabled ] || { echo "$name ATTACH-FAIL"; continue; }; fi
  sudo perf stat -a -e context-switches -o /tmp/cs.stat -- stress-ng --futex 8 -t 5 --metrics-brief --yaml /tmp/cs.y > /dev/null 2>&1
  ops=$(grep -m1 "bogo-ops-per-second-real-time:" /tmp/cs.y | awk '{printf "%d", $2}')
  cs=$(awk '/context-switches/{gsub(",","",$1); printf "%d", $1/5}' /tmp/cs.stat)
  echo "$name ops=$ops cs_per_s=$cs state=$(st)"
  [ -n "$pid" ] && { sudo kill -INT $pid; wait $pid; }
  for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
done; done
