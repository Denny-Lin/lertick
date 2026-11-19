#!/bin/bash
# Safety checks for sp3: fairness gate, CI test and edge cases, stress battery,
# 30-minute long run, cgroup weights.
cd ~/lertick-exp
F=sp3check; rm -rf $F; mkdir -p $F
log() { echo "$(date +%T) $*" >> $F/progress.log; }
B="scx/sp3/scx_lertick --min-train-samples 2"
REPS=3 ./gate.sh sp3 $B > $F/gate.txt 2>&1; log "gate rc=$?"
./cgp.sh $B > $F/cgroup.txt 2>&1; log "cgroup done"
cat > /tmp/sp3.sh <<EOS
#!/bin/bash
exec /home/ubuntu/lertick-exp/scx/sp3/scx_lertick --min-train-samples 2 "\$@"
EOS
chmod +x /tmp/sp3.sh
./edge-run.sh /tmp/sp3.sh; cp edge/progress.log $F/edge.txt; log "edge done"
./stress-run.sh sp3 $B; cp stress/sp3/progress.log $F/stress.txt; log "stress done"
./long-run.sh sp3 30 $B; cp long/sp3/progress.log $F/long.txt; log "long done"
log "all done"
