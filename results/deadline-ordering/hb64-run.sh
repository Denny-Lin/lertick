#!/bin/bash
# Targeted repro: hackbench -g 64 in a loop, 10-minute blocks, arms alternating.
cd ~/lertick-exp
OUT=hb64; rm -rf $OUT; mkdir -p $OUT
log() { echo "$(date +%T) $*" >> $OUT/progress.log; }
st() { cat /sys/kernel/sched_ext/state; }
for blk in 1 2 3; do for arm in "d50=scx/d50/scx_lertick --min-train-samples 2" "v2=scx/v2/scx_lertick"; do
  name=${arm%%=*}; cmd=${arm#*=}
  sudo dmesg -C
  sudo $cmd --exit-dump-len 4194304 > $OUT/$name-$blk.sched.log 2>&1 & pid=$!
  for _ in $(seq 100); do [ "$(st)" = enabled ] && break; sleep 0.1; done
  end=$(( $(date +%s) + 600 )); n=0; ej=0
  while [ $(date +%s) -lt $end ]; do
    rt-tests/hackbench -g 64 -l 2000 > /dev/null 2>&1; n=$((n+1))
    [ "$(st)" = enabled ] || { ej=1; break; }
  done
  log "$name block $blk: runs=$n ejected=$ej"
  sudo kill -INT $pid 2>/dev/null; wait $pid
  [ $ej = 1 ] && sudo dmesg > $OUT/$name-$blk.dmesg
  for _ in $(seq 50); do [ "$(st)" = disabled ] && break; sleep 0.1; done
done; done
log done
