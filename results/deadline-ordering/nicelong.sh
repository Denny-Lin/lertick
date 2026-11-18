#!/bin/bash
# Repeat the nice mix with one scheduler attached until ejection or the deadline.
# Usage: nicelong.sh LABEL MINUTES COMMAND...
set -u
cd ~/lertick-exp
label=$1; mins=$2; shift 2
OUT=long/$label; rm -rf $OUT; mkdir -p $OUT
log() { echo "$(date +%T) $*" >> $OUT/progress.log; }
state() { cat /sys/kernel/sched_ext/state; }
sudo dmesg -C
sudo "$@" > $OUT/sched.log 2>&1 & pid=$!
for _ in $(seq 100); do [ "$(state)" = enabled ] && break; sleep 0.1; done
end=$(( $(date +%s) + mins * 60 )); n=0
while [ $(date +%s) -lt $end ]; do
  n=$((n + 1))
  stress-ng --cpu 8 --timeout 30s >/dev/null 2>&1 & a=$!
  nice -n 19 stress-ng --cpu 8 --timeout 30s >/dev/null 2>&1 & b=$!
  sudo nice -n -5 stress-ng --cpu 2 --timeout 30s >/dev/null 2>&1 & c=$!
  wait $a $b $c  # not a bare wait: the scheduler is a background job too
  [ "$(state)" = enabled ] || { log "EJECTED in run $n"; break; }
done
log "final $(state) after $n runs"
sudo kill -INT $pid; wait $pid
sudo dmesg > $OUT/dmesg.txt
log "done"
