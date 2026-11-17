#!/bin/bash
# Nice-mix endurance: the release build (with the debug dump), then b7 (before the atomic clock and the lag clamp).
cd ~/lertick-exp
until grep -q "rel-check done" rel-check.log 2>/dev/null; do sleep 20; done
./nicelong.sh nice-rel 25 scx/rel/scx_lertick
./nicelong.sh nice-b7 25 scx/b7/scx_lertick
echo "$(date +%T) nicelong done" >> nicelong.log
