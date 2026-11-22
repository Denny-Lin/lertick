#!/bin/bash
# Multi-queue screen on 8 CPUs (and schbench on 4): high switch-rate loads,
# with the context switch rate; one line per arm and run.
cd ~/lertick-exp
st() { cat /sys/kernel/sched_ext/state; }
p99() { tac | awk -v k="$1" '/\* 99.0th/{v=$3} $0 ~ k {print v; exit}'; }
runs=$1; shift
for r in $(seq $runs); do for arm in $(printf '%s\n' "$@" | shuf); do name=${arm%%=*}; pid=
  if [ "$arm" != default ]; then cmd=${arm#*=}; sudo ${cmd//,/ } > /dev/null 2>&1 & pid=$!
    for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
    [ "$(st)" = enabled ] || { echo "$name ATTACH-FAIL"; continue; }; fi
  mp=$(perf bench sched messaging -g 10 -l 400 2>&1 | awk '/Total time/{print $3}')
  mt=$(perf bench sched messaging -t -g 10 -l 400 2>&1 | awk '/Total time/{print $3}')
  h16=$(rt-tests/hackbench -g 16 -l 1000 2>&1 | awk '/Time:/{print $2}')
  ht=$(rt-tests/hackbench -T -g 4 -l 2000 2>&1 | awk '/Time:/{print $2}')
  pp=$(perf bench sched pipe -l 100000 2>&1 | awk '/usecs\/op/{print $1}')
  o8=$(schbench/schbench -m 4 -t 2 -r 10 -w 2 2>&1)
  o4=$(taskset -c 0-3 schbench/schbench -m 4 -t 2 -r 10 -w 2 2>&1)
  echo "$name mp=$mp mt=$mt h16=$h16 ht=$ht pipe=$pp q8=$(echo "$o8" | p99 'Request Latencies') w8=$(echo "$o8" | p99 'Wakeup Latencies') q4=$(echo "$o4" | p99 'Request Latencies') w4=$(echo "$o4" | p99 'Wakeup Latencies') state=$(st)"
  [ -n "$pid" ] && { sudo kill -INT $pid; wait $pid; }
  for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
done; done
