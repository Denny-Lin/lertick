#!/bin/bash
# Cap sweep by CPU count: perf messaging processes, arms default h0 h8 h16 h24 h32 h48 h64.
cd ~/lertick-exp
A=""; for a in h0 qs8 h16 h24 h32 h48 h64; do A="$A $a=scx/$a/scx_lertick,--min-train-samples,2"; done
for c in 0-1 0-3 0-5; do ./msg-run.sh sw-$c $c 10 5 default $A; echo "$(date +%T) $c done" >> sweep.log; done
echo "$(date +%T) all done" >> sweep.log
