#!/bin/bash
# Build scx_lertick variants from /tmp/port.tgz. Usage: scx-build.sh NAME "DEFINE=VAL ..."
set -e
export PATH=$HOME/reference/llvm-20/bin:$HOME/reference/tool-bin:$HOME/.cargo/bin:$PATH
D=$HOME/reference/scx-main/scheds/experimental/scx_lertick
name=$1; shift
rm -rf $D; mkdir -p $D; tar xzf /tmp/port.tgz -C $(dirname $D) 2>/dev/null
# Fresh mtimes: tar keeps the archive's, and cargo would reuse a newer stale BPF object.
find $D -type f -exec touch {} +
for kv in $*; do k=${kv%%=*}; v=${kv#*=}; sed -i "s/^#define $k[[:space:]].*/#define $k ($v)/" $D/src/bpf/policy.h; grep -q "^#define $k ($v)$" $D/src/bpf/policy.h; done
cd $D; cargo build --release 2>&1 | grep -E "^error|error:" && exit 1
mkdir -p ~/lertick-exp/scx/$name; cp ~/reference/scx-main/target/release/scx_lertick ~/lertick-exp/scx/$name/
echo built $name
