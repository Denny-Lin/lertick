#!/bin/bash
# Release checks of the fork commit (fork-v5): catalog test, build, veristat,
# benchmarks against EEVDF and the PR build, fairness gate, edge cases with the
# CI test, stress battery and a one-hour long run.
set -u
cd ~/lertick-exp
F=final5; mkdir -p $F
log() { echo "$(date +%T) $*" >> $F/progress.log; }
export PATH=$HOME/reference/llvm-20/bin:$HOME/reference/tool-bin:$HOME/.cargo/bin:$PATH
W=$HOME/reference/scx-v5
[ -d $W ] || git -C $HOME/reference/scx-main worktree add -f $W fork-v5 > /dev/null 2>&1
git -C $W log --oneline -1 > $F/commit.txt
( cd $W && CARGO_TARGET_DIR=$HOME/reference/scx-main/target cargo test -p scx_catalog > $OLDPWD/$F/catalog-test.txt 2>&1 ); log "catalog test rc=$?"
( cd $W && touch scheds/experimental/scx_lertick/src/bpf/main.bpf.c && CARGO_TARGET_DIR=$HOME/reference/scx-main/target cargo build --release -p scx_lertick > $OLDPWD/$F/build.txt 2>&1 ); log "build rc=$?"
mkdir -p scx/v5 && cp $HOME/reference/scx-main/target/release/scx_lertick scx/v5/
obj=$(ls -t $HOME/reference/scx-main/target/release/build/scx_lertick-*/out/bpf.bpf.o $HOME/reference/scx-main/target/release/build/scx_lertick/*/out/bpf.bpf.o 2>/dev/null | head -1)
echo "$obj" > $F/veristat.txt
sudo $HOME/reference/linux-7.0/tools/testing/selftests/bpf/veristat $obj >> $F/veristat.txt 2>&1; log "veristat rc=$?"
B=scx/v5/scx_lertick
A="default v2=scx/v2/scx_lertick v5=$B"
./heldout.sh final5-cpu4 0,1,2,3 8 $A && log "bench cpu4 done"
./heldout.sh final5-cpu8 0,1,2,3,4,5,6,7 8 $A && log "bench cpu8 done"
REPS=3 ./gate.sh v5 $B > $F/gate.txt 2>&1; log "gate rc=$?"
./cgp.sh $B > $F/cgroup.txt 2>&1; log "cgroup done"
./edge-run.sh $B; cp edge/progress.log $F/edge.txt; log "edge done"
./stress-run.sh v5 $B; cp stress/v5/progress.log $F/stress.txt; log "stress done"
./long-run.sh v5 60 $B; cp long/v5/progress.log $F/long.txt; log "long done"
log "all done"
