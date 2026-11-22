#!/bin/bash
# cand-check.sh NAME: full safety checks of scx/NAME (gate, cgroup, edge, stress, 30-min long run).
cd ~/lertick-exp
N=$1; F=candcheck-$N; rm -rf $F; mkdir -p $F
log() { echo "$(date +%T) $*" >> $F/progress.log; }
B="scx/$N/scx_lertick --min-train-samples 2"
REPS=3 ./gate.sh $N $B > $F/gate.txt 2>&1; log "gate rc=$?"
./cgp.sh $B > $F/cgroup.txt 2>&1; log "cgroup done"
printf "#!/bin/bash\nexec /home/ubuntu/lertick-exp/scx/$N/scx_lertick --min-train-samples 2 \"\$@\"\n" > /tmp/cand-$N.sh; chmod +x /tmp/cand-$N.sh
./edge-run.sh /tmp/cand-$N.sh; cp edge/progress.log $F/edge.txt; log "edge done"
./stress-run.sh $N $B; cp stress/$N/progress.log $F/stress.txt; log "stress done"
./long-run.sh $N 30 $B; cp long/$N/progress.log $F/long.txt; log "long done"
log "all done"
