#!/bin/bash
# Fixed vs EEVDF on cpu, affinity, memory, kernelcompile: 2 blocks x 5 repetitions.
cd ~/lertick-exp
mkdir -p rest
arms=$(echo "default fixed online" | tr ' ' '\n' | shuf --random-source=<(yes 20261118))
rev=$(echo "$arms" | tac)
for block in 20261118 20261119; do
  [ $block = 20261118 ] && order=$arms || order=$rev
  for pol in $order; do
    name=base-$pol-$block; out=rest/$name
    [ -f rest/$name.tar.gz ] && continue
    rm -rf $out
    echo "$(date +%T) start $name" >> rest/progress.log
    if sudo python3 base/benchmarks/run_phase2.py --output $out --policies $pol \
        --workloads cpu,affinity,memory,kernelcompile --repetitions 5 --memory-mib 3000 \
        --kernel-source /home/ubuntu/reference/linux-7.0 --kernel-build /home/ubuntu/reference/linux7 \
        --perf /usr/bin/perf --seed $block --cpus 0,1,2,3 --bpf-runtime > $out.log 2>&1; then
      python3 summarize2.py base-$pol $out >> rest/summary.jsonl
      sudo tar czf rest/$name.tar.gz $out && sudo rm -rf $out
      echo "$(date +%T) ok $name $(du -sh rest/$name.tar.gz | cut -f1)" >> rest/progress.log
    else
      echo "$(date +%T) FAIL $name $(tail -1 $out.log)" >> rest/progress.log
    fi
    [ "$(cat /sys/kernel/sched_ext/state)" = disabled ] || { echo "$(date +%T) state not disabled, stop" >> rest/progress.log; exit 1; }
  done
done
echo "$(date +%T) all done" >> rest/progress.log
