#!/bin/bash
# Stability under a stress battery: attach one scheduler for the whole run,
# check every second that sched_ext stays enabled, and record each stressor.
# Usage: stress-run.sh LABEL COMMAND...
set -u
cd ~/lertick-exp
label=$1; shift
OUT=stress/$label; rm -rf $OUT; mkdir -p $OUT
log() { echo "$(date +%T) $*" >> $OUT/progress.log; }
state() { cat /sys/kernel/sched_ext/state; }
sudo dmesg -C
sudo "$@" > $OUT/sched.log 2>&1 & pid=$!
for _ in $(seq 100); do [ "$(state)" = enabled ] && break; sleep 0.1; done
[ "$(state)" = enabled ] || { log "FAIL attach"; exit 1; }
( while sleep 1; do s=$(state); [ "$s" = enabled ] || echo "$(date +%T) $s"; done ) > $OUT/state.log &
mon=$!
T=45
run() { local name=$1; shift; log "start $name"; "$@" > $OUT/$name.txt 2>&1; log "end $name rc=$? state=$(state)"; }
run cpu        stress-ng --cpu 0 --timeout ${T}s --metrics-brief
run nice-mix   bash -c "stress-ng --cpu 8 --timeout ${T}s & nice -n 19 stress-ng --cpu 8 --timeout ${T}s & nice -n -5 stress-ng --cpu 2 --timeout ${T}s & wait"
run fork       stress-ng --fork 16 --timeout ${T}s --metrics-brief
run switch     stress-ng --switch 16 --timeout ${T}s --metrics-brief
run pipe       stress-ng --pipe 16 --timeout ${T}s --metrics-brief
run futex      stress-ng --futex 16 --timeout ${T}s --metrics-brief
run yield      stress-ng --yield 16 --timeout ${T}s --metrics-brief
run affinity   stress-ng --affinity 16 --timeout ${T}s --metrics-brief
run sched      stress-ng --sched 8 --timeout ${T}s --metrics-brief
run mixed      stress-ng --cpu 4 --fork 4 --switch 4 --pipe 4 --futex 4 --timer 4 --vm 2 --vm-bytes 256M --timeout 90s --metrics-brief
run hackbench  rt-tests/hackbench -g 64 -l 2000
run msg-big    perf bench sched messaging -g 40 -l 1000
run kbuild-all bash -c "find kbuild/kernel -name '*.o' -delete; make -s -C ~/reference/linux-7.0 O=$HOME/lertick-exp/kbuild -j16 kernel/ > /dev/null"
kill $mon
log "final state $(state)"
sudo kill -INT $pid; wait $pid
sudo dmesg | grep -i "sched_ext\|scx\|stall\|watchdog" > $OUT/dmesg.txt
log "done"
