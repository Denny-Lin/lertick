#!/bin/bash
# perf bench sched messaging (processes, -g 10 -l 400) only: arms alternate
# every round, each attached for a block of runs. Usage: msg-run.sh LABEL CPUS ROUNDS RUNS ARM...
set -u
cd ~/lertick-exp
label=$1; cpus=$2; rounds=$3; runs=$4; shift 4
OUT=msg/$label; rm -rf $OUT; mkdir -p $OUT
st() { cat /sys/kernel/sched_ext/state; }
for r in $(seq $rounds); do
  for arm in $(printf '%s\n' "$@" | shuf); do
    name=${arm%%=*}; pid=
    if [ "$arm" != default ]; then
      cmd=${arm#*=}; sudo ${cmd//,/ } > /dev/null 2>&1 & pid=$!
      for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
    fi
    for i in $(seq $runs); do
      taskset -c $cpus perf bench sched messaging -g 10 -l 400 2>&1 | grep -oE "Total time: [0-9.]+" | awk -v a=$name '{print a, $3}' >> $OUT/times.txt
    done
    [ -n "$pid" ] && { [ "$(st)" = enabled ] || echo "fallback $name $r" >> $OUT/fallback.txt; sudo kill -INT $pid; wait $pid; }
    for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
  done
done
echo done >> $OUT/done
