#!/bin/bash
# fair32.sh ARM...: 3 CPU hogs pinned to 2 CPUs for 20 s; CPU time of each hog.
cd ~/lertick-exp
st() { cat /sys/kernel/sched_ext/state; }
for arm in "$@"; do name=${arm%%=*}; pid=
  if [ "$arm" != default ]; then cmd=${arm#*=}; sudo ${cmd//,/ } > /dev/null 2>&1 & pid=$!
    for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done; fi
  pids=""; for i in 1 2 3; do taskset -c 0,1 sh -c 'while :; do :; done' & pids="$pids $!"; done
  sleep 20
  t=$(for p in $pids; do awk '{printf "%.1f ", ($14+$15)/100}' /proc/$p/stat; done)
  kill $pids 2>/dev/null; wait $pids 2>/dev/null
  echo "$name cpu_s: $t state=$(st)"
  [ -n "$pid" ] && { sudo kill -INT $pid; wait $pid; }
  for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
done
