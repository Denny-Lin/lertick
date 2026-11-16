#!/bin/bash
# cgroup weight check. Usage: cg_test.sh [LOADER MODE]   (no arguments: EEVDF)
# Case 1: two services pinned to one CPU with CPUWeight 200 and 100; expect a 2:1 CPU split.
# Case 2: two busy tasks in service A against one in service B, equal weights; expect A:B = 1:1.
set -u
CPU=${CPU:-5}; SECS=${SECS:-10}
state() { cat /sys/kernel/sched_ext/state; }
pid=
if [ $# -ge 2 ]; then
  sudo $1 --mode $2 --track-all --duration 0 > /tmp/cg-loader.log 2>&1 & pid=$!
  for _ in $(seq 100); do [ "$(state)" = enabled ] && break; sleep 0.1; done
  [ "$(state)" = enabled ] || { echo "attach failed"; exit 1; }
fi
usage() { awk '/usage_usec/ {print $2}' /sys/fs/cgroup/system.slice/$1.service/cpu.stat; }
run_case() { # name wa wb na nb
  sudo systemd-run -q --unit=lt-cg-a -p CPUWeight=$2 taskset -c $CPU sh -c "for i in \$(seq $4); do (while :; do :; done) & done; wait"
  sudo systemd-run -q --unit=lt-cg-b -p CPUWeight=$3 taskset -c $CPU sh -c "for i in \$(seq $5); do (while :; do :; done) & done; wait"
  sleep 2; a0=$(usage lt-cg-a); b0=$(usage lt-cg-b); sleep $SECS; a1=$(usage lt-cg-a); b1=$(usage lt-cg-b)
  sudo systemctl stop lt-cg-a lt-cg-b
  awk -v n="$1" -v a=$((a1 - a0)) -v b=$((b1 - b0)) 'BEGIN { printf "%s: A %.2f s, B %.2f s, A/B %.2f\n", n, a/1e6, b/1e6, (b ? a/b : 0) }'
}
run_case "weight 200:100, 1:1 tasks" 200 100 1 1
run_case "weight 100:100, 2:1 tasks" 100 100 2 1
# Case 3: task weights inside one cgroup, nice 0 against nice 5; EEVDF gives about 1024:335 = 3.06.
sudo systemd-run -q --unit=lt-cg-a taskset -c $CPU sh -c "(while :; do :; done) & (exec nice -n 5 sh -c 'while :; do :; done') & wait"
sleep 2; pids=$(pgrep -f "while :; do :; done" | head -10)
t0=$(for p in $pids; do echo "$p $(awk '{print $14+$15, $19}' /proc/$p/stat)"; done); sleep $SECS
t1=$(for p in $pids; do echo "$p $(awk '{print $14+$15, $19}' /proc/$p/stat)"; done)
sudo systemctl stop lt-cg-a
join <(echo "$t0" | sort) <(echo "$t1" | sort) | awk '{d=$4-$2; if ($3==0) a+=d; else b+=d} END { printf "nice 0 vs 5 in one cgroup: ratio %.2f\n", (b ? a/b : 0) }'
if [ -n "$pid" ]; then
  [ "$(state)" = enabled ] || echo "FALLBACK during test"
  sudo kill -INT $pid; wait $pid
fi
