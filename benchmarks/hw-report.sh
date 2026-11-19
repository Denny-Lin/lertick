#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Compare scx_lertick with the kernel's default scheduler (EEVDF) on this
# machine and write a report to paste into a GitHub issue.
#
#   sudo benchmarks/hw-report.sh /path/to/scx_lertick [REPS] [SCHEDULER OPTIONS...]
#
# Needs a kernel with sched_ext and no other sched_ext scheduler running.
# Uses the tools found among: perf, hackbench (rt-tests), schbench,
# stress-ng; a missing tool only skips its tests. The two schedulers take
# turns in a random order in each repetition (default 5). Takes about 3
# minutes per repetition on an 8-CPU machine. The report holds the CPU model,
# topology, CPU capacities and kernel version, and no task names or PIDs.
set -u

SCHED=${1:?usage: hw-report.sh /path/to/scx_lertick [REPS] [SCHEDULER OPTIONS...]}
REPS=${2:-5}
shift $(($# < 2 ? $# : 2))
SCHED_OPTS=("$@")
OUT=lertick-report-$(date +%Y%m%d-%H%M%S)
STATE=/sys/kernel/sched_ext/state
NCPU=$(nproc)
ALL=0-$((NCPU - 1))

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)"; exit 1; }
[ -r $STATE ] || { echo "this kernel has no sched_ext"; exit 1; }
[ "$(cat $STATE)" = disabled ] || { echo "another sched_ext scheduler is running"; exit 1; }
[ -x "$SCHED" ] || { echo "$SCHED is not executable"; exit 1; }
mkdir -p $OUT

have() { command -v "$1" > /dev/null 2>&1; }
wait_state() { for _ in $(seq 150); do [ "$(cat $STATE)" = "$1" ] && return 0; sleep 0.1; done; return 1; }

# Hardware and kernel, without anything about the user's tasks.
{
	echo "kernel: $(uname -r) $(uname -m)"
	lscpu | grep -E "^(Model name|Architecture|CPU\(s\)|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\)|L3 cache|NUMA node\(s\))"
	echo -n "cpu_capacity:"
	for c in /sys/devices/system/cpu/cpu[0-9]*; do
		[ -r $c/cpu_capacity ] && echo -n " ${c##*cpu}=$(cat $c/cpu_capacity)"
	done
	echo
	echo "smt: $(cat /sys/devices/system/cpu/smt/active 2>/dev/null || echo unknown)"
	echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
	echo "scheduler: $("$SCHED" --version 2>/dev/null | head -1) ${SCHED_OPTS[*]}"
} > $OUT/system.txt

# Each test prints "name value"; lower is better unless the name ends in /s.
run_tests() {
	if have perf; then
		perf bench sched messaging -g 10 -l 400 2>/dev/null |
			awk '/Total time/ {print "perf-messaging-processes_s", $3}'
		perf bench sched messaging -t -g 10 -l 400 2>/dev/null |
			awk '/Total time/ {print "perf-messaging-threads_s", $3}'
		perf bench sched pipe -l 200000 2>/dev/null |
			awk '/usecs\/op/ {print "perf-pipe_us", $1}'
	fi
	if have hackbench; then
		hackbench -T -g 4 -l 2000 2>/dev/null | awk '/^Time/ {print "hackbench-threads_s", $2}'
		hackbench -g 16 -l 1000 2>/dev/null | awk '/^Time/ {print "hackbench-16-groups_s", $2}'
	fi
	if have schbench; then
		schbench -m 4 -t $((NCPU / 4 > 1 ? NCPU / 4 : 2)) -r 15 -w 3 2>&1 | tac | awk '
			/\* 99.0th/ && !w {v = $3}
			/Wakeup Latencies/ && !w {print "schbench-wakeup-p99_us", v; w = 1}
			/\* 99.0th/ && !q {u = $3}
			/Request Latencies/ && !q {print "schbench-request-p99_us", u; q = 1}'
	fi
	if have stress-ng; then
		for s in futex fork mq matrix; do
			stress-ng --$s $NCPU --timeout 10s --metrics-brief --yaml /tmp/lertick-$s.y > /dev/null 2>&1
			awk -v n="stress-ng-$s" '/bogo-ops-per-second-real-time:/ {print n "_ops/s", $2; exit}' /tmp/lertick-$s.y
		done
	fi
}

for rep in $(seq $REPS); do
	for arm in $(printf 'eevdf\nlertick\n' | shuf); do
		pid=
		if [ $arm = lertick ]; then
			"$SCHED" "${SCHED_OPTS[@]}" > $OUT/sched-$rep.log 2>&1 &
			pid=$!
			wait_state enabled || { echo "scx_lertick did not attach"; kill -INT $pid; exit 1; }
		fi
		run_tests | sed "s/^/$arm /" >> $OUT/results.txt
		if [ -n "$pid" ]; then
			[ "$(cat $STATE)" = enabled ] || echo "rep $rep: scx_lertick was ejected" >> $OUT/events.txt
			kill -INT $pid
			wait $pid
			wait_state disabled
		fi
		echo "repetition $rep of $REPS: $arm done"
	done
done

python3 - $OUT <<'PY' > $OUT/report.md
import collections, statistics, sys
out = sys.argv[1]
d = collections.defaultdict(lambda: collections.defaultdict(list))
for line in open(f"{out}/results.txt"):
    arm, name, value = line.split()
    d[name][arm].append(float(value))
print("## scx_lertick against EEVDF\n")
print("```")
print(open(f"{out}/system.txt").read().rstrip())
print("```\n")
print("| Test | EEVDF | scx_lertick | Lertick better by |")
print("|---|---|---|---|")
for name, arms in d.items():
    if "eevdf" not in arms or "lertick" not in arms:
        continue
    e, l = statistics.median(arms["eevdf"]), statistics.median(arms["lertick"])
    higher = name.endswith("/s")
    pct = ((l / e - 1) if higher else (1 - l / e)) * 100 if e else float("nan")
    print(f"| {name} | {e:.4g} | {l:.4g} | {pct:+.1f}% |")
try:
    print("\nEvents:\n\n" + open(f"{out}/events.txt").read())
except FileNotFoundError:
    print("\nscx_lertick stayed attached in every repetition.")
PY
echo "report: $OUT/report.md"
