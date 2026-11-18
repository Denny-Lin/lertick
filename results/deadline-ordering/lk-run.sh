#!/bin/bash
# Long-keep test: alternated arms on 4 CPUs; keeps benchmark output and BSS.
# Usage: lk-run.sh LABEL REPS "arm=binary[,arg...]" ...
set -u
cd ~/lertick-exp
label=$1; reps=$2; shift 2
OUT=lk/$label; rm -rf $OUT; mkdir -p $OUT
log() { echo "$(date +%T) $*" >> $OUT/progress.log; }
TS="taskset -c 0,1,2,3"
run() {
  arm=$1; cmd=$2; name=$3; shift 3
  sudo ${cmd//,/ } > $OUT/$arm-$name.sched.log 2>&1 & pid=$!
  for _ in $(seq 100); do [ "$(cat /sys/kernel/sched_ext/state)" = enabled ] && break; sleep 0.1; done
  [ "$(cat /sys/kernel/sched_ext/state)" = enabled ] || { log "$arm did not attach"; }
  "$@" > $OUT/$arm-$name.out 2>&1
  id=$(sudo bpftool -j map show | python3 -c 'import json,sys; print([m["id"] for m in json.load(sys.stdin) if m.get("name","").endswith(".bss")][-1])')
  sudo bpftool -j map dump id $id > $OUT/$arm-$name.bss 2>&1
  sudo kill -INT $pid; wait $pid
}
for rep in $(seq $reps); do
  for spec in "$@"; do
    arm=${spec%%=*}; cmd=${spec#*=}
    run $arm $cmd sb-m4t2-$rep  $TS schbench/schbench -m 4 -t 2 -r 15 -w 3
    run $arm $cmd sb-m1t1-$rep  $TS schbench/schbench -m 1 -t 1 -r 15 -w 3
    run $arm $cmd msg-proc-$rep $TS perf bench sched messaging -g 20 -l 2000
    log "$arm rep $rep done"
  done
done
log "all done"
