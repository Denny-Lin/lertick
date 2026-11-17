#!/bin/bash
# Long stability run: attach one scheduler and cycle a mixed load until the
# deadline, checking every second that sched_ext stays enabled. Stops at the
# first ejection and keeps the kernel log. Usage: long-run.sh LABEL MINUTES COMMAND...
set -u
cd ~/lertick-exp
label=$1; mins=$2; shift 2
OUT=long/$label; rm -rf $OUT; mkdir -p $OUT
log() { echo "$(date +%T) $*" >> $OUT/progress.log; }
state() { cat /sys/kernel/sched_ext/state; }
sudo dmesg -C
sudo "$@" > $OUT/sched.log 2>&1 & pid=$!
for _ in $(seq 100); do [ "$(state)" = enabled ] && break; sleep 0.1; done
[ "$(state)" = enabled ] || { log "FAIL attach"; exit 1; }
( while sleep 1; do s=$(state); [ "$s" = enabled ] || echo "$(date +%T) $s"; done ) > $OUT/state.log &
mon=$!
end=$(( $(date +%s) + mins * 60 )); cycle=0
T=30
step() { local name=$1; shift; "$@" >> $OUT/$name.txt 2>&1; [ "$(state)" = enabled ] || { log "EJECTED during $name, cycle $cycle"; return 1; }; }
while [ $(date +%s) -lt $end ]; do
  cycle=$((cycle + 1))
  step nice-mix bash -c "stress-ng --cpu 8 --timeout ${T}s & nice -n 19 stress-ng --cpu 8 --timeout ${T}s & nice -n -5 stress-ng --cpu 2 --timeout ${T}s & wait" || break
  step hb16 rt-tests/hackbench -g 16 -l 1000 || break
  step hb32 rt-tests/hackbench -g 32 -l 1000 || break
  step hb64 rt-tests/hackbench -g 64 -l 1000 || break
  step msg perf bench sched messaging -g 40 -l 500 || break
  step sb schbench/schbench -m 4 -t 2 -r 15 -w 3 || break
  step mixed stress-ng --cpu 4 --fork 4 --switch 4 --pipe 4 --futex 4 --timer 4 --vm 2 --vm-bytes 256M --yield 4 --timeout ${T}s || break
  step kbuild bash -c "find kbuild/kernel -name '*.o' -delete; make -s -C ~/reference/linux-7.0 O=$HOME/lertick-exp/kbuild -j16 kernel/ > /dev/null" || break
  log "cycle $cycle ok"
done
kill $mon
log "final state $(state) after $cycle cycles"
sudo kill -INT $pid; wait $pid
sudo dmesg > $OUT/dmesg.txt
log "done"
