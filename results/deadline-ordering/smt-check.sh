#!/bin/bash
# Safety checks of the SMT build (smt): with siblings declared
# 0-1,2-3,4-5,6-7, then the nice-mix gate without the override.
cd ~/lertick-exp
F=smtcheck; rm -rf $F; mkdir -p $F
log() { echo "$(date +%T) $*" >> $F/progress.log; }
O="--min-train-samples 2 --smt-siblings 0-1,2-3,4-5,6-7"
B="scx/smt/scx_lertick $O"
REPS=3 ./gate.sh smt-on $B > $F/gate.txt 2>&1; log "gate rc=$?"
./cgp.sh $B > $F/cgroup.txt 2>&1; log "cgroup done"
printf '#!/bin/bash\nexec /home/ubuntu/lertick-exp/scx/smt/scx_lertick %s "$@"\n' "$O" > /tmp/smt.sh; chmod +x /tmp/smt.sh
./edge-run.sh /tmp/smt.sh; cp edge/progress.log $F/edge.txt; log "edge done"
./stress-run.sh smt $B; cp stress/smt/progress.log $F/stress.txt; log "stress done"
./long-run.sh smt 30 $B; cp long/smt/progress.log $F/long.txt; log "long done"
REPS=3 ./gate.sh smt-off scx/smt/scx_lertick --min-train-samples 2 > $F/gate-sym.txt 2>&1; log "gate-sym rc=$?"
REPS=3 ./gate.sh smt-both scx/smt/scx_lertick --min-train-samples 2 --smt-siblings 0-1,2-3,4-5,6-7 --cpu-capacity 0-3=1024,4-7=512 > $F/gate-both.txt 2>&1; log "gate-both rc=$?"
log "all done"
