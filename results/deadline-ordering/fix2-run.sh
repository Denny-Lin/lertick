#!/bin/bash
# Atomic clock, second stall test: hackbench -g 16 only, 40 repetitions, 1 s watchdog builds.
cd ~/lertick-exp
while pgrep -f "fix-run.sh|gate.sh" > /dev/null; do sleep 15; done
rm -rf stall/atomic16
GROUPS_LIST="16 16 16" ARMS="old=scx/f-old-w1/scx_lertick new=scx/f-new-w1/scx_lertick" ./stall8.sh atomic16 40
echo "$(date +%T) all done" >> fix2-run.log
