#!/bin/bash
# Screen wakeup preemption (kickA, kickB) and the 5 ms continuation cap (keepC)
# against the current algorithm (base) and EEVDF. Builds live in ~/lertick-exp/kick.
# Starts after more-run.sh; stops if any build fails a 10-second load test.
set -u
cd ~/lertick-exp
while pgrep -f more-run.sh > /dev/null; do sleep 30; done
log() { echo "$(date +%T) $*" >> kick-run.log; }
for n in base kickA kickB keepC; do
  sudo timeout -s INT 10 kick/$n/scx_lertick --mode online --track-all --duration 0 > kick/$n-smoke.log 2>&1 &
  sleep 5; st=$(cat /sys/kernel/sched_ext/state); wait
  sleep 1
  [ "$st" = enabled ] || { log "FAIL smoke $n ($st)"; exit 1; }
  log "smoke $n ok"
done
A=""
for n in base kickA kickB keepC; do A="$A $n=kick/$n/scx_lertick,--mode,online,--track-all,--duration,0"; done
./heldout.sh kick-cpu4 0,1,2,3 3 default $A && log "cpu4 done" &&
./heldout.sh kick-cpu8 0,1,2,3,4,5,6,7 3 default $A && log "all done" || log "FAIL run"
