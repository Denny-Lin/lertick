#!/bin/bash
# Longer kernel-compile test: rebuild every object under kernel/ (not just
# kernel/sched) from a clean state in a private build tree.
# Usage: kbuild-long.sh LABEL CPUS REPS ARM...
# ARM is "default" or NAME=COMMAND (a scheduler command line, commas for spaces).
set -u
cd ~/lertick-exp
label=$1; cpus=$2; reps=$3; shift 3
OUT=kbuild-long/$label; mkdir -p $OUT
n=$(( $(echo $cpus | tr -cd , | wc -c) + 1 ))
TS="taskset -c $cpus"
log() { echo "$(date +%T) $*" >> $OUT/progress.log; }
state() { cat /sys/kernel/sched_ext/state; }
wait_state() { for _ in $(seq 1 150); do [ "$(state)" = "$1" ] && return 0; sleep 0.1; done; return 1; }
kbuild() {
  find kbuild/kernel -name '*.o' -o -name '*.a' | xargs -r rm -f
  sync; sleep 2
  local s=$(date +%s.%N)
  $TS make -s -C ~/reference/linux-7.0 O=$HOME/lertick-exp/kbuild -j$n kernel/ > /dev/null 2>&1
  local rc=$?
  echo "elapsed $(echo "$(date +%s.%N) - $s" | bc) rc $rc"
}
# Warm the page cache once so the first timed arm is not penalised.
kbuild > $OUT/warmup.txt 2>&1
for rep in $(seq 0 $((reps - 1))); do
  for arm in $(printf '%s\n' "$@" | shuf --random-source=<(yes "$label$rep")); do
    name=${arm%%=*}; d=$OUT/$rep-$name
    [ -f $d/done ] && continue
    mkdir -p $d; pid=
    [ "$(state)" = disabled ] || { log "state not disabled before $rep-$name, stop"; exit 1; }
    if [ "$arm" != default ]; then
      cmd=${arm#*=}; sudo ${cmd//,/ } > $d/sched.log 2>&1 &
      pid=$!
      wait_state enabled || { log "FAIL attach $rep-$name"; sudo kill -INT $pid; wait $pid; exit 1; }
    fi
    kbuild > $d/kbuild.txt 2>&1
    if [ -n "$pid" ]; then
      [ "$(state)" = enabled ] || { log "FALLBACK $rep-$name"; touch $d/fallback; }
      sudo kill -INT $pid 2>/dev/null; wait $pid
      wait_state disabled || { log "FAIL detach $rep-$name"; exit 1; }
    fi
    touch $d/done; log "ok $rep-$name $(cat $d/kbuild.txt)"
  done
done
log "all done"
