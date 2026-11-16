#!/bin/bash
# B confirmation: 2 blocks x 5 repetitions; arms shuffled, then reversed.
cd ~/lertick-exp
mkdir -p confirm
BPF=/home/ubuntu/reference/scx-v1.1.2/target/release/scx_bpfland
arms=$(echo "default:base fixed:base online:base online:B scx_bpfland:base" | tr ' ' '\n' | shuf --random-source=<(yes 20261116))
rev=$(echo "$arms" | tac)
for block in 20261116 20261117; do
  [ $block = 20261116 ] && order=$arms || order=$rev
  for arm in $order; do
    pol=${arm%%:*}; b=${arm##*:}; name=$b-$pol-$block; out=confirm/$name
    [ -f confirm/$name.tar.gz ] && continue
    rm -rf $out
    extra=(); [ $pol = scx_bpfland ] && extra=(--extra-scheduler scx_bpfland=$BPF)
    echo "$(date +%T) start $name" >> confirm/progress.log
    if sudo python3 $b/benchmarks/run_phase2.py --output $out --policies $pol "${extra[@]}" \
        --workloads mixed,multicore,shortburst,wakeheavy --repetitions 5 \
        --seed $block --cpus 0,1,2,3 --bpf-runtime > $out.log 2>&1; then
      python3 summarize.py $b-$pol $out >> confirm/summary.jsonl
      sudo tar czf confirm/$name.tar.gz $out && sudo rm -rf $out
      echo "$(date +%T) ok $name $(du -sh confirm/$name.tar.gz | cut -f1)" >> confirm/progress.log
    else
      echo "$(date +%T) FAIL $name $(tail -1 $out.log)" >> confirm/progress.log
    fi
    [ "$(cat /sys/kernel/sched_ext/state)" = disabled ] || { echo "$(date +%T) state not disabled, stop" >> confirm/progress.log; exit 1; }
  done
done
echo "$(date +%T) all done" >> confirm/progress.log
