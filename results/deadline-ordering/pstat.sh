#!/bin/bash
# Context switches and migrations during perf messaging (processes), per arm.
# Usage: pstat.sh CPUS RUNS ARM...
cd ~/lertick-exp
cpus=$1; runs=$2; shift 2
st() { cat /sys/kernel/sched_ext/state; }
for r in $(seq $runs); do
  for arm in "$@"; do
    name=${arm%%=*}; pid=
    if [ "$arm" != default ]; then cmd=${arm#*=}; sudo ${cmd//,/ } > /dev/null 2>&1 & pid=$!
      for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done; fi
    taskset -c $cpus perf bench sched messaging -g 10 -l 400 > /dev/null 2>&1   # warm-up
    out=$(sudo perf stat -x, -e context-switches,cpu-migrations,task-clock taskset -c $cpus perf bench sched messaging -g 10 -l 400 2>&1 >/tmp/pm.out)
    t=$(grep -oE "Total time: [0-9.]+" /tmp/pm.out | awk '{print $3}')
    cs=$(echo "$out" | grep context-switches | cut -d, -f1); mig=$(echo "$out" | grep cpu-migrations | cut -d, -f1); tc=$(echo "$out" | grep task-clock | cut -d, -f1)
    echo "$name $t $cs $mig $tc"
    [ -n "$pid" ] && { sudo kill -INT $pid; wait $pid; }
    for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
  done
done
