#!/bin/bash
# Quick A/B on schbench -m4 -t2 (4 CPUs): arms alternate, one run per attach.
cd ~/lertick-exp
runs=$1; shift
st() { cat /sys/kernel/sched_ext/state; }
for r in $(seq $runs); do for arm in "$@"; do
  name=${arm%%=*}; pid=
  if [ "$arm" != default ]; then cmd=${arm#*=}; sudo ${cmd//,/ } > /dev/null 2>&1 & pid=$!
    for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done; fi
  out=$(taskset -c 0-3 schbench/schbench -m 4 -t 2 -r 15 -w 3 2>&1)
  w=$(echo "$out" | awk '/Wakeup Latencies/{s=1} s&&/\* 99.0th/{print $3; exit}' )
  w=$(echo "$out" | tac | awk '/\* 99.0th/{v=$3} /Wakeup Latencies/{print v; exit}')
  q=$(echo "$out" | tac | awk '/\* 99.0th/{v=$3} /Request Latencies/{print v; exit}')
  rps=$(echo "$out" | grep -oE "average rps: [0-9.]+" | tail -1 | awk '{print $3}')
  echo "$name $w $q $rps"
  [ -n "$pid" ] && { sudo kill -INT $pid; wait $pid; }
  for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
done; done
