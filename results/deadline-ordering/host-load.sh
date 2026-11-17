#!/bin/bash
# Log the Mac host's load and its busiest processes every 10 s from START to
# END (epoch seconds). Usage: host-load.sh OUTFILE START END
now() { date +%s; }
while [ "$(now)" -lt "$2" ]; do sleep 10; done
while [ "$(now)" -lt "$3" ]; do
  echo "== $(date +%s) $(sysctl -n vm.loadavg)"
  top -l 1 -n 5 -o cpu -stats cpu,command | tail -5
  sleep 10
done > "$1"
