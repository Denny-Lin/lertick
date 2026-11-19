#!/bin/bash
# Sync-stacking counters: one attach per workload, bss dumped after.
cd ~/lertick-exp
OUT=$1; BIN=$2; rm -rf $OUT; mkdir -p $OUT
st() { cat /sys/kernel/sched_ext/state; }
run() { name=$1; shift
  sudo $BIN --min-train-samples 2 > $OUT/$name.log 2>&1 & pid=$!
  for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
  "$@" > $OUT/$name.out 2>&1
  id=$(sudo bpftool -j map show | python3 -c 'import json,sys; print([m["id"] for m in json.load(sys.stdin) if m.get("name","").endswith(".bss")][-1])')
  sudo bpftool -j map dump id $id > $OUT/$name.bss 2>&1
  sudo kill -INT $pid; wait $pid
}
T8="taskset -c 0-7"; T4="taskset -c 0-3"
run msgproc8 bash -c "for i in 1 2 3; do $T8 perf bench sched messaging -g 10 -l 400; done"
run msgthr8 bash -c "for i in 1 2 3; do $T8 perf bench sched messaging -t -g 10 -l 400; done"
run hb16 $T8 rt-tests/hackbench -g 16 -l 1000
run sbm4t2 $T4 schbench/schbench -m 4 -t 2 -r 15 -w 3
run pipe $T8 perf bench sched pipe -l 200000
echo done > $OUT/done
