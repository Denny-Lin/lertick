#!/bin/bash
# Step 1 of making the slice prediction useful: measure, per workload on 4
# CPUs, how well the learned model, the burst average and the last burst
# predict a completed burst, how many bursts outlast the fixed slice and the
# cap, and how often a slice ends a run. Then start tab-run.sh.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> meas-run.log; }
until grep -qE "^[0-9:]+ (all done|FAIL)" floor-run.log 2>/dev/null; do sleep 20; done
cp /tmp/port-meas.tgz /tmp/port.tgz; /tmp/scx-build.sh meas > /tmp/meas.build.log 2>&1 || { log "FAIL build"; tail -20 /tmp/meas.build.log >> meas-run.log; ./tab-run.sh; exit 1; }
OUT=meas; rm -rf $OUT; mkdir -p $OUT
TS="taskset -c 0,1,2,3"
run() {
  name=$1; shift
  sudo scx/meas/scx_lertick > $OUT/$name.sched.log 2>&1 & pid=$!
  for _ in $(seq 100); do [ "$(cat /sys/kernel/sched_ext/state)" = enabled ] && break; sleep 0.1; done
  "$@" > $OUT/$name.out 2>&1
  id=$(sudo bpftool -j map show | python3 -c 'import json,sys; print([m["id"] for m in json.load(sys.stdin) if m.get("name","").endswith(".bss")][-1])')
  sudo bpftool map dump id $id > $OUT/$name.bss 2>&1
  sudo kill -INT $pid; wait $pid
  log "$name done"
}
run msg-proc   $TS perf bench sched messaging -g 20 -l 2000
run msg-thread $TS perf bench sched messaging -t -g 20 -l 2000
run hb-thread  $TS rt-tests/hackbench -T -g 4 -l 2000
run hb-g16     $TS rt-tests/hackbench -g 16 -l 1000
run sb-m4t2    $TS schbench/schbench -m 4 -t 2 -r 15 -w 3
run sb-m1t1    $TS schbench/schbench -m 1 -t 1 -r 15 -w 3
run kbuild     bash -c "find kbuild/kernel -name '*.o' -delete; $TS make -s -C ~/reference/linux-7.0 O=$HOME/lertick-exp/kbuild -j8 kernel/ > /dev/null"
run mix        $TS stress-ng --cpu 2 --switch 2 --pipe 2 --futex 2 --timer 2 --timeout 20s
log "all done"
./tab-run.sh
