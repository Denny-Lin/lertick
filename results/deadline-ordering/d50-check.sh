#!/bin/bash
# Safety checks for d50: fairness gate, CI test and edge cases, stress battery,
# 30-minute long run, cgroup weights.
cd ~/lertick-exp
F=d50check; rm -rf $F; mkdir -p $F
log() { echo "$(date +%T) $*" >> $F/progress.log; }
B="scx/d50/scx_lertick --min-train-samples 2"
REPS=3 ./gate.sh d50 $B > $F/gate.txt 2>&1; log "gate rc=$?"
./cgp.sh $B > $F/cgroup.txt 2>&1; log "cgroup done"
cat > /tmp/d50.sh <<EOS
#!/bin/bash
exec /home/ubuntu/lertick-exp/scx/d50/scx_lertick --min-train-samples 2 "\$@"
EOS
chmod +x /tmp/d50.sh
./edge-run.sh /tmp/d50.sh; cp edge/progress.log $F/edge.txt; log "edge done"
./stress-run.sh d50 $B; cp stress/d50/progress.log $F/stress.txt; log "stress done"
./long-run.sh d50 30 $B; cp long/d50/progress.log $F/long.txt; log "long done"
log "all done"
