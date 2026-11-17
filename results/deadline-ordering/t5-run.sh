#!/bin/bash
# Validate the 5 s watchdog build (~/lertick-exp/t5/build): load test, injected
# watchdog tests, stall stress, then held-out and kernel compile on 4 and 8 CPUs.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> t5-run.log; }
B=t5/build/scx_lertick
sudo timeout -s INT 6 $B --mode online --track-all --duration 0 > t5/smoke.log 2>&1 &
sleep 3; st=$(cat /sys/kernel/sched_ext/state); wait; sleep 1
[ "$st" = enabled ] || { log "FAIL smoke ($st)"; exit 1; }
grep -q '"timeout_ms":5000' t5/smoke.log || { log "FAIL timeout not 5000"; exit 1; }
log "smoke ok, timeout 5000"
(cd t5 && sudo python3 tests/validate_watchdog_flags.py --output ~/lertick-exp/t5/watchdog-flags > ~/lertick-exp/t5/watchdog-flags.log 2>&1) \
  && log "watchdog flags ok" || { log "FAIL watchdog flags"; exit 1; }
[ "$(cat /sys/kernel/sched_ext/state)" = disabled ] || { log "FAIL state after watchdog test"; exit 1; }
ARMS="default base=kick/base/scx_lertick t5=$B" ./stall-test.sh s3 10 && log "stress done"
A="default base=kick/base/scx_lertick,--mode,online,--track-all,--duration,0 t5=$B,--mode,online,--track-all,--duration,0"
./heldout.sh t5-cpu4 0,1,2,3 5 $A && log "heldout cpu4 done" &&
./heldout.sh t5-cpu8 0,1,2,3,4,5,6,7 5 $A && log "heldout cpu8 done" &&
./kbuild-long.sh t5-cpu4 0,1,2,3 5 $A && log "kbuild cpu4 done" &&
./kbuild-long.sh t5-cpu8 0,1,2,3,4,5,6,7 5 $A && log "all done" || log "FAIL run"
