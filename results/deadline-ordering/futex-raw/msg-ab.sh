#!/bin/bash
# msg-ab.sh REPS ARM...: perf bench sched messaging (processes, threads) on 8 CPUs.
cd ~/lertick-exp
st() { cat /sys/kernel/sched_ext/state; }
reps=$1; shift
for r in $(seq $reps); do for arm in $(printf '%s\n' "$@" | shuf); do name=${arm%%=*}; pid=
  if [ "$arm" != default ]; then cmd=${arm#*=}; sudo ${cmd//,/ } > /dev/null 2>&1 & pid=$!
    for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
    [ "$(st)" = enabled ] || { echo "$name ATTACH-FAIL"; continue; }; fi
  p=$(perf bench sched messaging -g 10 -l 400 2>&1 | awk '/Total time/{print $3}')
  t=$(perf bench sched messaging -t -g 10 -l 400 2>&1 | awk '/Total time/{print $3}')
  echo "$name proc=$p thr=$t state=$(st)"
  [ -n "$pid" ] && { sudo kill -INT $pid; wait $pid; }
  for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
done; done
