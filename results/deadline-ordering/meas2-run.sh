#!/bin/bash
# Model accuracy, old model (meas) against the average-start, decaying-step
# model (m2), same workloads as meas-run.sh, run twice each in turn.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> meas2-run.log; }
cp /tmp/port-m2.tgz /tmp/port.tgz; /tmp/scx-build.sh m2 > /tmp/m2.build.log 2>&1 || { log "FAIL build"; grep -iE "error" /tmp/m2.build.log | tail -10 >> meas2-run.log; exit 1; }
TS="taskset -c 0,1,2,3"
run() {
  arm=$1; name=$2; shift 2
  OUT=meas2/$arm; mkdir -p $OUT
  sudo scx/$arm/scx_lertick > $OUT/$name.sched.log 2>&1 & pid=$!
  for _ in $(seq 100); do [ "$(cat /sys/kernel/sched_ext/state)" = enabled ] && break; sleep 0.1; done
  "$@" > $OUT/$name.out 2>&1
  id=$(sudo bpftool -j map show | python3 -c 'import json,sys; print([m["id"] for m in json.load(sys.stdin) if m.get("name","").endswith(".bss")][-1])')
  sudo bpftool map dump id $id > $OUT/$name.bss 2>&1
  sudo kill -INT $pid; wait $pid
}
rm -rf meas2
for rep in 1 2; do
  for arm in meas m2; do
    run $arm msg-proc-$rep   $TS perf bench sched messaging -g 20 -l 2000
    run $arm hb-g16-$rep     $TS rt-tests/hackbench -g 16 -l 1000
    run $arm sb-m4t2-$rep    $TS schbench/schbench -m 4 -t 2 -r 15 -w 3
    run $arm sb-m1t1-$rep    $TS schbench/schbench -m 1 -t 1 -r 15 -w 3
    run $arm kbuild-$rep     bash -c "find kbuild/kernel -name '*.o' -delete; $TS make -s -C ~/reference/linux-7.0 O=$HOME/lertick-exp/kbuild -j8 kernel/ > /dev/null"
    log "$arm rep $rep done"
  done
done
log "all done"
