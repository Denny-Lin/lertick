#!/bin/bash
# hbt.sh ARM...: hackbench -T -g 4 on 8 CPUs, 3 runs per attach, counters after.
cd ~/lertick-exp; mkdir -p hbt
st() { cat /sys/kernel/sched_ext/state; }
for a in "$@"; do
  sudo scx/$a/scx_lertick --min-train-samples 2 > /dev/null 2>&1 & pid=$!
  for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
  t=$(for i in 1 2 3; do taskset -c 0-7 rt-tests/hackbench -T -g 4 -l 2000 | grep -oE "Time: [0-9.]+" | cut -d" " -f2; done | tr "\n" " ")
  id=$(sudo bpftool -j map show | python3 -c 'import json,sys; print([m["id"] for m in json.load(sys.stdin) if m.get("name","").endswith(".bss")][-1])')
  sudo bpftool -j map dump id $id > hbt/$a.bss
  sudo kill -INT $pid; wait $pid
  echo "$a times: $t"
  grep -oE "\"(nr_pp_sync|nr_pp_pick_local|nr_pp_pick_shared|nr_sync_ok|nr_local_dispatches|nr_shared_dispatches)\":[0-9]+" hbt/$a.bss | tr "\n" " "; echo
  python3 - hbt/$a.bss <<'PY'
import re,sys
t=open(sys.argv[1]).read()
m=re.search(r'"pp_n":\[([0-9,]*)\]',t)
if m:
    g=lambda k:[int(x) for x in re.search(r'"%s":\[([0-9,]*)\]'%k,t).group(1).split(",")]
    n,w,pr,e=g("pp_n"),g("pp_wait_ns"),g("pp_pred_ns"),g("pp_err_ns")
    for i,nm in ((1,"local"),(2,"shared")):
        if n[i]: print(" ",nm,"n",n[i],"wait %.2f pred %.2f err %.2f ms"%(w[i]/n[i]/1e6,pr[i]/n[i]/1e6,e[i]/n[i]/1e6))
PY
done
