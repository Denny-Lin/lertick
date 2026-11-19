#!/bin/bash
# Safety checks of the main-line hybrid port (hyb): with capacities declared
# 0-3=1024,4-7=512, then the nice-mix gate without the override.
cd ~/lertick-exp
F=hybcheck; rm -rf $F; mkdir -p $F
log() { echo "$(date +%T) $*" >> $F/progress.log; }
O="--min-train-samples 2 --cpu-capacity 0-3=1024,4-7=512"
B="scx/hyb/scx_lertick $O"
REPS=3 ./gate.sh hyb-asym $B > $F/gate.txt 2>&1; log "gate rc=$?"
./cgp.sh $B > $F/cgroup.txt 2>&1; log "cgroup done"
printf '#!/bin/bash\nexec /home/ubuntu/lertick-exp/scx/hyb/scx_lertick %s "$@"\n' "$O" > /tmp/hyb.sh; chmod +x /tmp/hyb.sh
./edge-run.sh /tmp/hyb.sh; cp edge/progress.log $F/edge.txt; log "edge done"
./stress-run.sh hyb $B; cp stress/hyb/progress.log $F/stress.txt; log "stress done"
./long-run.sh hyb 30 $B; cp long/hyb/progress.log $F/long.txt; log "long done"
REPS=3 ./gate.sh hyb-sym scx/hyb/scx_lertick --min-train-samples 2 > $F/gate-sym.txt 2>&1; log "gate-sym rc=$?"
log "all done"
