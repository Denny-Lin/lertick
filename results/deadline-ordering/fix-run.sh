#!/bin/bash
# Atomic clock fix: build, stall stress with 1 s watchdog builds, gate.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> fix-run.log; }
until grep -q "all done\|FAIL" r11-run.log 2>/dev/null; do sleep 30; done
for v in old-w1 new-w1 new; do cp /tmp/port-$v.tgz /tmp/port.tgz; /tmp/scx-build.sh f-$v || { log "FAIL build $v"; exit 1; }; done
log "built"
ARMS="old=scx/f-old-w1/scx_lertick new=scx/f-new-w1/scx_lertick" ./stall8.sh atomic 12
log "stall done"
REPS=4 ./gate.sh b9 scx/f-new/scx_lertick >> gate-fix.txt 2>&1
log "all done"
