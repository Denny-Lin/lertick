#!/bin/bash
# Root-cgroup weight test: one nice -20 CPU hog in the root cgroup next to the
# nice mix in the session cgroup, 4 CPUs, 40 s. Prints the scheduler state and
# each group's bogo ops. Usage: rootcg.sh NAME [COMMAND...]  (no command: EEVDF)
set -u
cd ~/lertick-exp
name=$1; shift
st() { cat /sys/kernel/sched_ext/state; }
pid=
if [ $# -gt 0 ]; then
  sudo dmesg -C
  sudo "$@" > /tmp/rootcg-$name.log 2>&1 & pid=$!
  for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
fi
rm -f /tmp/rc*.y
sudo taskset -c 0-3 nice -n -20 stress-ng --cpu 1 --cpu-method int64 --timeout 40s --metrics-brief --yaml /tmp/rcroot.y >/dev/null 2>&1 &
sleep 0.5
for p in $(pgrep -f "stress-ng --cpu 1 --cpu-method int64"); do echo $p | sudo tee /sys/fs/cgroup/cgroup.procs > /dev/null; done
taskset -c 0-3 stress-ng --cpu 4 --cpu-method int64 --timeout 38s --metrics-brief --yaml /tmp/rc0.y >/dev/null 2>&1 &
taskset -c 0-3 nice -n 19 stress-ng --cpu 4 --cpu-method int64 --timeout 38s --metrics-brief --yaml /tmp/rc19.y >/dev/null 2>&1 &
sleep 41; s=$(st)
echo "$name ($s): root-nice-20 $(grep -m1 bogo-ops: /tmp/rcroot.y | awk '{print $2}') nice0 $(grep -m1 bogo-ops: /tmp/rc0.y | awk '{print $2}') nice19 $(grep -m1 bogo-ops: /tmp/rc19.y | awk '{print $2}') $(sudo dmesg | grep -o 'failed to run for [0-9.]*s' | head -1)"
[ -n "$pid" ] && { sudo kill -INT $pid; wait $pid; }
