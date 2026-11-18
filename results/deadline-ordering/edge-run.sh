#!/bin/bash
# Edge cases before release, run against one scx build after the long run.
# Usage: edge-run.sh BINARY. Results in edge/progress.log; each test records the
# scheduler's exit code and whether the watchdog fired.
set -u
cd ~/lertick-exp
B=$1
OUT=edge; rm -rf $OUT; mkdir -p $OUT
log() { echo "$(date +%T) $*" >> $OUT/progress.log; }
st() { cat /sys/kernel/sched_ext/state; }
up() { sudo dmesg -C; sudo $B "$@" > $OUT/sched-$name.log 2>&1 & pid=$!; for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done; }
down() { sudo kill -INT $pid 2>/dev/null; wait $pid; rc=$?; f=$(sudo dmesg | grep -o 'failed to run for [0-9.]*s\|runnable task stall' | head -1); log "$name: rc=$rc state-before-stop=$s ${f:-no-stall}"; }
# (run after the long run finishes)
N=$(nproc)

# 1. The sched-ext/scx CI test, verbatim: aggressive stress with affinity churn, SIGTERM after 45 s, exit code must be 0.
for r in 1 2 3; do
  name=ci$r
  stress-ng -t 31 --aggressive -M -c $N -f $N --affinity 1 --affinity-delay 1 --affinity-pin > /dev/null 2>&1 & sp=$!
  sudo dmesg -C
  sudo timeout --foreground --preserve-status 45 $B -v > $OUT/sched-$name.log 2>&1; rc=$?
  kill $sp 2>/dev/null; wait $sp 2>/dev/null
  log "$name: rc=$rc $(sudo dmesg | grep -o 'failed to run for [0-9.]*s' | head -1)"
done

# 2. Attach and detach 20 times, with a short load each time.
fails=0
for r in $(seq 20); do
  sudo $B > /dev/null 2>&1 & pid=$!
  for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
  [ "$(st)" = enabled ] || fails=$((fails + 1))
  rt-tests/hackbench -g 4 -l 200 > /dev/null 2>&1
  sudo kill -INT $pid; wait $pid || fails=$((fails + 1))
  for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
  [ "$(st)" = disabled ] || fails=$((fails + 1))
done
log "attach-detach x20: failures=$fails"


# 4. cgroup churn: busy tasks moved between 8 cgroups every 0.2 s, weights changed, cgroups removed at the end.
name=cgroup-churn; up
sudo mkdir -p /sys/fs/cgroup/lt-churn
echo "+cpu" | sudo tee /sys/fs/cgroup/lt-churn/cgroup.subtree_control > /dev/null
for i in $(seq 8); do sudo mkdir -p /sys/fs/cgroup/lt-churn/g$i; done
stress-ng --cpu $N --timeout 60s > /dev/null 2>&1 & sp=$!
nice -n 19 stress-ng --cpu 4 --timeout 60s > /dev/null 2>&1 & sp2=$!
end=$(( $(date +%s) + 55 ))
while [ $(date +%s) -lt $end ]; do
  for p in $(pgrep -f "stress-ng-cpu"); do echo $p | sudo tee /sys/fs/cgroup/lt-churn/g$((RANDOM % 8 + 1))/cgroup.procs > /dev/null 2>&1; done
  echo $((RANDOM % 1000 + 1)) | sudo tee /sys/fs/cgroup/lt-churn/g$((RANDOM % 8 + 1))/cpu.weight > /dev/null
  sleep 0.2
done
wait $sp $sp2; s=$(st)
for i in $(seq 8); do sudo rmdir /sys/fs/cgroup/lt-churn/g$i; done; sudo rmdir /sys/fs/cgroup/lt-churn
down

# 5. SCHED_IDLE and SCHED_BATCH tasks next to normal ones.
name=idle-batch; up
chrt -i 0 stress-ng --cpu 4 --timeout 40s > /dev/null 2>&1 & a=$!
chrt -b 0 stress-ng --cpu 4 --timeout 40s > /dev/null 2>&1 & b=$!
stress-ng --cpu 4 --timeout 40s > /dev/null 2>&1 & c=$!
rt-tests/hackbench -g 8 -l 2000 > /dev/null 2>&1
wait $a $b $c; s=$(st); down

# 6. A real-time task hogging one CPU while sched_ext tasks share it.
name=rt-hog; up
sudo chrt -f 50 taskset -c 2 stress-ng --cpu 1 --timeout 30s > /dev/null 2>&1 & a=$!
stress-ng --cpu $N --timeout 30s > /dev/null 2>&1 & b=$!
rt-tests/hackbench -g 8 -l 2000 > /dev/null 2>&1
wait $a $b; s=$(st); down

# 7. Process storm: fork, exec and clone.
name=proc-storm; up
stress-ng --fork 16 --exec 8 --clone 8 --vfork 8 --timeout 60s > /dev/null 2>&1
s=$(st); down

# 8. Many threads and sleeps: thread churn and timers.
name=threads-timers; up
stress-ng --pthread 8 --timer 8 --nanosleep 8 --clock 4 --timeout 45s > /dev/null 2>&1
s=$(st); down
log "edge done"
