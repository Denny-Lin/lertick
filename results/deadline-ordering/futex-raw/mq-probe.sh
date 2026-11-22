#!/bin/bash
# mq-probe.sh BIN: per workload, the 50 ms periods (or decisions) spent at 1, 2, 4, 8 queues.
cd ~/lertick-exp
st() { cat /sys/kernel/sched_ext/state; }
dump() { for id in $(sudo bpftool map list 2>/dev/null | awk '/\.bss/{print $1}' | tr -d :); do
  sudo bpftool -j map dump id $id 2>/dev/null; done | python3 -c '
import json,sys
txt=sys.stdin.read()
out={}
dec=json.JSONDecoder(); i=0
while i<len(txt):
    try: obj,j=dec.raw_decode(txt,i)
    except Exception: break
    i=j
    while i<len(txt) and txt[i].isspace(): i+=1
    def walk(x):
        if isinstance(x,dict):
            for k,y in x.items():
                if k in ("mq_hist","mq_level","nr_mq_switch","mq_cost","nr_mq_flips","nr_mq_changes","nr_mq_decisions","mq_minn"): out[k]=y
                walk(y)
        elif isinstance(x,list):
            for y in x: walk(y)
    walk(obj)
print(" ".join(f"{k}={v}" for k,v in sorted(out.items())))'; }
for w in idle futex8 hackbench msg schbench4; do
  sudo $1 --min-train-samples 2 > /dev/null 2>&1 & pid=$!
  for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
  case $w in
    idle) sleep 3;;
    futex8) stress-ng --futex 8 -t 5 >/dev/null 2>&1;;
    hackbench) for i in 1 2 3; do rt-tests/hackbench -g 16 -l 1000 >/dev/null 2>&1; done;;
    msg) for i in 1 2 3 4 5; do perf bench sched messaging -g 10 -l 400 >/dev/null 2>&1; done;;
    schbench4) taskset -c 0-3 schbench/schbench -m 4 -t 2 -r 6 -w 1 >/dev/null 2>&1;;
  esac
  echo "$w $(dump)"
  sudo kill -INT $pid; wait $pid
  for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
done
