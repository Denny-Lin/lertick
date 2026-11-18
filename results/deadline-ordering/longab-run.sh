#!/bin/bash
# Alternating 60-minute long runs, d50 and v2, twice each, with a large dump.
cd ~/lertick-exp
log() { echo "$(date +%T) $*" >> longab.log; }
for i in 1 2; do
  ./long-run.sh d50-$i 60 scx/d50/scx_lertick --min-train-samples 2 --exit-dump-len 4194304; log "d50-$i $(tail -2 long/d50-$i/progress.log | head -1)"
  ./long-run.sh v2-$i 60 scx/v2/scx_lertick --exit-dump-len 4194304; log "v2-$i $(tail -2 long/v2-$i/progress.log | head -1)"
done
log "all done"
