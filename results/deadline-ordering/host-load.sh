#!/bin/bash
# Log the Mac host's load and its busiest processes every 10 s until stopped.
# Usage: host-load.sh OUTFILE
while true; do
  echo "== $(date +%s) $(sysctl -n vm.loadavg)"
  top -l 1 -n 5 -o cpu -stats cpu,command | tail -5
  sleep 10
done > "$1"
