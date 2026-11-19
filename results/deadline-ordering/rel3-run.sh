#!/bin/bash
# Release candidate rel3 (per-CPU stats + stealable CPU queues): full held-out
# set against EEVDF and the current release (v5), then the safety checks.
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> rel3-run.log; }
A="default v5=scx/v5/scx_lertick,--min-train-samples,2 rel3=scx/rel3/scx_lertick,--min-train-samples,2"
log start
./heldout.sh rel3-cpu8 0,1,2,3,4,5,6,7 6 $A && log "cpu8 done" || log "cpu8 FAIL"
./heldout.sh rel3-cpu4 0,1,2,3 6 $A && log "cpu4 done" || log "cpu4 FAIL"
F=rel3check; rm -rf $F; mkdir -p $F
B="scx/rel3/scx_lertick --min-train-samples 2"
REPS=3 ./gate.sh rel3 $B > $F/gate.txt 2>&1; log "gate rc=$?"
./cgp.sh $B > $F/cgroup.txt 2>&1; log "cgroup done"
printf '#!/bin/bash\nexec /home/ubuntu/lertick-exp/scx/rel3/scx_lertick --min-train-samples 2 "$@"\n' > /tmp/rel3.sh; chmod +x /tmp/rel3.sh
./edge-run.sh /tmp/rel3.sh; cp edge/progress.log $F/edge.txt; log "edge done"
./stress-run.sh rel3 $B; cp stress/rel3/progress.log $F/stress.txt; log "stress done"
./long-run.sh rel3 30 $B; cp long/rel3/progress.log $F/long.txt; log "long done"
log "all done"
