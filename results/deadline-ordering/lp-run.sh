#!/bin/bash
# Learned wakeup preemption (lp): build off (lp0) and on (lp), gate lp, then
# screen against lp0 and EEVDF on 4 and 8 CPUs, 3 repetitions.
set -u
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> lp-run.log; }
until grep -qE "^[0-9:]+ (all done|FAIL)" ab-run.log 2>/dev/null; do sleep 20; done
for v in lp0 lp; do cp /tmp/port-$v.tgz /tmp/port.tgz; /tmp/scx-build.sh $v > /tmp/$v.build.log 2>&1 || { log "FAIL build $v"; tail -20 /tmp/$v.build.log >> lp-run.log; exit 1; }; done
log "built"
REPS=4 ./gate.sh lp scx/lp/scx_lertick > gate-lp.txt 2>&1
grep -q "nice-mix passed 4/4" gate-lp.txt || { log "FAIL gate"; exit 1; }
N=$(nproc); ok=0
for r in 1 2; do stress-ng -t 31 --aggressive -M -c $N -f $N --affinity 1 --affinity-delay 1 --affinity-pin > /dev/null 2>&1 & sp=$!; sudo timeout --foreground --preserve-status 45 scx/lp/scx_lertick > /tmp/lpci$r.log 2>&1 && ok=$((ok+1)); kill $sp 2>/dev/null; wait $sp 2>/dev/null; done
log "gate done, ci $ok/2"
A="default lp0=scx/lp0/scx_lertick lp=scx/lp/scx_lertick"
./heldout.sh lp-cpu4 0,1,2,3 3 $A && log "cpu4 done" &&
./heldout.sh lp-cpu8 0,1,2,3,4,5,6,7 3 $A && log "all done" || log "FAIL run"
