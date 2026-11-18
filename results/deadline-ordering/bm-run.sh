#!/bin/bash
# Bimodal wakeup p99 on 4 CPUs: repeat schbench -m4 -t2 with one build, keep
# each run's output and BSS counters.
set -u
cd ~/lertick-exp
B=$1; N=$2; OUT=bm; rm -rf $OUT; mkdir -p $OUT
for r in $(seq $N); do
  sudo $B > $OUT/$r.sched.log 2>&1 & pid=$!
  for _ in $(seq 100); do [ "$(cat /sys/kernel/sched_ext/state)" = enabled ] && break; sleep 0.1; done
  taskset -c 0,1,2,3 schbench/schbench -m 4 -t 2 -r 20 -w 3 > $OUT/$r.out 2>&1
  id=$(sudo bpftool -j map show | python3 -c 'import json,sys; print([m["id"] for m in json.load(sys.stdin) if m.get("name","").endswith(".bss")][-1])')
  sudo bpftool -j map dump id $id > $OUT/$r.bss 2>&1
  sudo kill -INT $pid; wait $pid
done
echo done > $OUT/done
