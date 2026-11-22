#!/bin/bash
# 4-CPU screen: futex 1 and 4 CPUs, schbench m1t1 and m4t2, hackbench threads, all on CPUs 0-3.
cd ~/lertick-exp
ops() { grep -m1 "bogo-ops-per-second-real-time:" $1 | awk '{printf "%d", $2}'; }
st() { cat /sys/kernel/sched_ext/state; }
p99() { tac | awk -v k="$1" '/\* 99.0th/{v=$3} $0 ~ k {print v; exit}'; }
runs=$1; shift
for r in $(seq $runs); do for arm in $(printf '%s\n' "$@" | shuf); do
  name=${arm%%=*}; pid=
  if [ "$arm" != default ]; then cmd=${arm#*=}; sudo ${cmd//,/ } > /dev/null 2>&1 & pid=$!
    for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
    [ "$(st)" = enabled ] || { echo "$name ATTACH-FAIL"; continue; }; fi
  TS="taskset -c 0-3"
  $TS stress-ng --futex 4 -t 6 --metrics-brief --yaml /tmp/f4.y >/dev/null 2>&1; f4=$(ops /tmp/f4.y)
  taskset -c 0 stress-ng --futex 1 -t 4 --metrics-brief --yaml /tmp/f1.y >/dev/null 2>&1; f1=$(ops /tmp/f1.y)
  o1=$($TS schbench/schbench -m 1 -t 1 -r 10 -w 2 2>&1)
  o2=$($TS schbench/schbench -m 4 -t 2 -r 12 -w 3 2>&1)
  hb=$($TS rt-tests/hackbench -T -g 4 -l 2000 2>&1 | awk '/Time:/{print $2}')
  echo "$name f4=$f4 f1=$f1 q1=$(echo "$o1" | p99 'Request Latencies') w1=$(echo "$o1" | p99 'Wakeup Latencies') q2=$(echo "$o2" | p99 'Request Latencies') w2=$(echo "$o2" | p99 'Wakeup Latencies') rps=$(echo "$o2" | grep -oE 'average rps: [0-9.]+' | tail -1 | awk '{print $3}') hb=$hb state=$(st)"
  [ -n "$pid" ] && { sudo kill -INT $pid; wait $pid; }
  for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
done; done
