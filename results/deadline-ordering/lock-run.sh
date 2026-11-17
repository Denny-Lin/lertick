#!/bin/bash
# Is the schbench -m 4 -t 2 loss tied to its per-CPU spin lock? Run it with
# and without -L (no locking) under EEVDF and the scx build, on CPUs 0-3.
set -u
cd ~/lertick-exp
OUT=diag/lock; mkdir -p $OUT
state() { cat /sys/kernel/sched_ext/state; }
for rep in 0 1 2; do
  for arm in default scx; do
    for lk in lock nolock; do
      pid=
      if [ $arm = scx ]; then
        sudo scx/b0/scx_lertick > $OUT/$rep-$arm-$lk.sched 2>&1 & pid=$!
        for _ in $(seq 100); do [ "$(state)" = enabled ] && break; sleep 0.1; done
      fi
      L=; [ $lk = nolock ] && L=-L
      taskset -c 0-3 schbench/schbench -m 4 -t 2 -r 20 -w 3 $L > $OUT/$rep-$arm-$lk.txt 2>&1
      if [ -n "$pid" ]; then
        [ "$(state)" = enabled ] || touch $OUT/$rep-$arm-$lk.fallback
        sudo kill -INT $pid; wait $pid
      fi
    done
  done
done
echo done > $OUT/done
