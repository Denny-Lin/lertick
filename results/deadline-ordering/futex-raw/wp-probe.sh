#!/bin/bash
# wp-probe.sh BIN: preemption and queue choices during 4-CPU schbench -m4 -t2 and 8-CPU futex.
cd ~/lertick-exp
st() { cat /sys/kernel/sched_ext/state; }
dump() { for id in $(sudo bpftool map list 2>/dev/null | awk '/\.bss/{print $1}' | tr -d :); do sudo bpftool -j map dump id $id 2>/dev/null; done | python3 -c '
import json,sys
txt=sys.stdin.read(); out={}; dec=json.JSONDecoder(); i=0
keys=("nr_wp_pre","nr_wp_queue","nr_wq_own","nr_wq_shared","nr_pq_own","nr_pq_shared","nr_pq_fair_shared","nr_keep_pcpu_yield","nr_cpuq_deferred","nr_waker_preempts")
while i<len(txt):
    try: obj,j=dec.raw_decode(txt,i)
    except Exception: break
    i=j
    while i<len(txt) and txt[i].isspace(): i+=1
    def walk(x):
        if isinstance(x,dict):
            for k,y in x.items():
                if k in keys: out[k]=y
                walk(y)
        elif isinstance(x,list):
            for y in x: walk(y)
    walk(obj)
print(" ".join(f"{k}={out[k]}" for k in keys if k in out))'; }
for w in schbench4 futex8; do
  sudo $1 --min-train-samples 2 > /dev/null 2>&1 & pid=$!
  for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
  case $w in
    schbench4) taskset -c 0-3 schbench/schbench -m 4 -t 2 -r 8 -w 2 >/dev/null 2>&1;;
    futex8) stress-ng --futex 8 -t 5 >/dev/null 2>&1;;
  esac
  echo "$w $(dump)"
  sudo kill -INT $pid; wait $pid
  for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
done
