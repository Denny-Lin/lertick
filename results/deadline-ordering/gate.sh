#!/bin/bash
# Safety gate before any screening: nice mix at full load (3 runs, the
# watchdog must not fire), nice-weight shares on 4 CPUs, and one schbench
# -m 4 -t 2 run on 4 CPUs with migrations. Usage: gate.sh NAME COMMAND...
set -u
cd ~/lertick-exp
name=$1; shift
st() { cat /sys/kernel/sched_ext/state; }
up() { sudo "$@" > /dev/null 2>&1 & pid=$!; for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done; }
down() { sudo kill -INT $pid 2>/dev/null; wait $pid 2>/dev/null; }
ok=0
for rep in $(seq 1 ${REPS:-3}); do
  sudo dmesg -C; up "$@"
  stress-ng --cpu 8 --timeout 30s >/dev/null 2>&1 & nice -n 19 stress-ng --cpu 8 --timeout 30s >/dev/null 2>&1 &
  nice -n -5 stress-ng --cpu 2 --timeout 30s >/dev/null 2>&1 & sleep 32
  s=$(st); down; [ "$s" = enabled ] && ok=$((ok+1))
  echo "$name nice-mix $rep: $s $(sudo dmesg | grep -o 'failed to run for [0-9.]*s')"
done
up "$@"; rm -f /tmp/g*.y
taskset -c 0-3 nice -n -5 stress-ng --cpu 2 --cpu-method int64 --timeout 20s --metrics-brief --yaml /tmp/gm5.y >/dev/null 2>&1 &
taskset -c 0-3 stress-ng --cpu 4 --cpu-method int64 --timeout 20s --metrics-brief --yaml /tmp/g0.y >/dev/null 2>&1 &
taskset -c 0-3 nice -n 19 stress-ng --cpu 4 --cpu-method int64 --timeout 20s --metrics-brief --yaml /tmp/g19.y >/dev/null 2>&1 &
sleep 22; s=$(st)
echo "$name fairness ($s): nice-5 $(grep -m1 bogo-ops: /tmp/gm5.y | awk '{print $2}') nice0 $(grep -m1 bogo-ops: /tmp/g0.y | awk '{print $2}') nice19 $(grep -m1 bogo-ops: /tmp/g19.y | awk '{print $2}')"
sudo perf stat -e cpu-migrations -o /tmp/gm.perf -- taskset -c 0-3 schbench/schbench -m 4 -t 2 -r 15 -w 3 > /tmp/gsb.txt 2>&1
s=$(st); down
echo "$name m4t2 ($s): wake99 $(grep -A6 'Wakeup Latencies' /tmp/gsb.txt | grep 99.0th | tail -1 | awk '{print $3}') req99 $(grep -A6 'Request Latencies' /tmp/gsb.txt | grep 99.0th | tail -1 | awk '{print $3}') migrations $(grep migrations /tmp/gm.perf | awk '{print $1}')"
echo "$name nice-mix passed $ok/${REPS:-3}"
