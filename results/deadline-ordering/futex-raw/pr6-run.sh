#!/bin/bash
# After the screen: build m3 (final tree), safety-check it, then 6-run full set vs EEVDF and hs.
S=$(dirname "$0")
cd $S
./vm.sh 'cd ~/lertick-exp; sudo ./scx/m3/scx_lertick > /tmp/m3.log 2>&1 & sleep 4; cat /sys/kernel/sched_ext/state; stress-ng --futex 8 -t 5 --metrics-brief 2>&1 | grep -E "futex +[0-9]"; sudo pkill -INT -f scx/m3/scx_lertick; sleep 3' </dev/null
echo "$(date +%T) safety"
./vm.sh 'cd ~/lertick-exp && ./cand-check.sh m3' </dev/null
./vm.sh 'cd ~/lertick-exp/candcheck-m3 && cat progress.log && grep -hE "passed|weight|ratio|rc=|failures|state" gate.txt cgroup.txt edge.txt stress.txt long.txt | head -60' </dev/null
echo "$(date +%T) full set, 6 runs"
./vm.sh 'cd ~/lertick-exp; A="default hs=scx/hs/scx_lertick m3=scx/m3/scx_lertick"; sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null; ./heldout-fx.sh pr6-cpu8 0,1,2,3,4,5,6,7 6 $A && ./heldout-fx.sh pr6-cpu4 0,1,2,3 6 $A; grep -hE "RETRY|FAIL" heldout/pr6-cpu*/progress.log; echo "== 8 CPUs"; python3 cmpfx.py heldout/pr6-cpu8 hs m3; echo "== 4 CPUs"; python3 cmpfx.py heldout/pr6-cpu4 hs m3' </dev/null
echo "$(date +%T) all done"
