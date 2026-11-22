#!/bin/bash
# futex screen: arms alternate; per run: stress-ng futex 8 on 8 CPUs, futex 1 on 1 CPU,
# schbench -m4 -t2 on 4 CPUs (wakeup/request p99, rps), hackbench threads on 4 CPUs.
cd ~/lertick-exp
runs=$1; shift
ops() { grep -m1 "bogo-ops-per-second-real-time:" $1 | awk '{printf "%d", $2}'; }
st() { cat /sys/kernel/sched_ext/state; }
for r in $(seq $runs); do for arm in $(printf '%s\n' "$@" | shuf); do
  name=${arm%%=*}; pid=
  if [ "$arm" != default ]; then cmd=${arm#*=}; sudo ${cmd//,/ } > /dev/null 2>&1 & pid=$!
    for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
    [ "$(st)" = enabled ] || { echo "$name ATTACH-FAIL"; continue; }; fi
  taskset -c 0-7 stress-ng --futex 8 -t 8 --metrics-brief --yaml /tmp/f8.y >/dev/null 2>&1; f8=$(ops /tmp/f8.y)
  taskset -c 0 stress-ng --futex 1 -t 5 --metrics-brief --yaml /tmp/f1.y >/dev/null 2>&1; f1=$(ops /tmp/f1.y)
  out=$(taskset -c 0-3 schbench/schbench -m 4 -t 2 -r 12 -w 3 2>&1)
  w=$(echo "$out" | tac | awk '/\* 99.0th/{v=$3} /Wakeup Latencies/{print v; exit}')
  q=$(echo "$out" | tac | awk '/\* 99.0th/{v=$3} /Request Latencies/{print v; exit}')
  rps=$(echo "$out" | grep -oE "average rps: [0-9.]+" | tail -1 | awk '{print $3}')
  out8=$(schbench/schbench -m 4 -t 2 -r 10 -w 2 2>&1)
  q8=$(echo "$out8" | tac | awk '/\* 99.0th/{v=$3} /Request Latencies/{print v; exit}')
  r8=$(echo "$out8" | grep -oE "average rps: [0-9.]+" | tail -1 | awk '{print $3}')
  hb=$(taskset -c 0-3 rt-tests/hackbench -T -g 4 -l 2000 2>&1 | awk '/Time:/{print $2}')
  ok=$(st)
  echo "$name f8=$f8 f1=$f1 sbw=$w sbq=$q rps=$rps sbq8=$q8 rps8=$r8 hb=$hb state=$ok"
  [ -n "$pid" ] && { sudo kill -INT $pid; wait $pid; }
  for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
done; done
