# Workload and benchmark boundary

`burst_workload.c` provides controlled CPU bursts with real sleeps and a separate
continuously runnable CPU role. It calibrates work batches with
`CLOCK_THREAD_CPUTIME_ID`, then measures each burst's actual CPU time. Burst mode
initially sleeps for 1ms after calibration to establish a verified wake boundary.
On Linux the task comm is `sprout_burst` or `sprout_cpu`; JSONL includes Linux PID
and TID.

```sh
make workload
build/burst_workload --duration 3 --burst-us 2000 --sleep-us 8000 \
  --phase-burst-us 500 > burst.jsonl
build/burst_workload --duration 3 --role cpu > cpu.jsonl
# Run separate instances concurrently on an explicitly selected Linux CPU:
build/burst_workload --duration 3 --burst-us 300 --sleep-us 700 --cpu 0
```

`--phase-burst-us` changes the target halfway through wall time. `--policy-ext`
explicitly requests Linux scheduler policy 7 for partial-switch tests; successful
policy selection alone does not prove an active BPF scheduler. `--cpu` applies
Linux affinity. Defaults require neither flag.

Burst samples contain `target_cpu_ns`, measured `cpu_ns`, `wall_ns`,
`response_ns` (actual wake lateness after the sleep deadline), `completion_ns`
(deadline-to-burst-completion), and work `units`. Summaries report measured CPU
and wall time and `throughput_units_per_s`. These are workload measurements;
response latency is a wakeup lateness metric, not an application request SLO.
CPU role emits only calibration and final records so logging does not add sleep
episodes during its measured run. Burst-mode logging may affect measurements.

Use identical contention and duration for default Linux, fixed sched_ext and
online sched_ext trials. Retain raw JSONL, scheduler telemetry, environment
manifest and failure logs. A small VM comparison is a deployment sanity check.
The [protocol](../docs/planning/BENCHMARK_PLAN.md) defines the broader experiments;
no performance improvement follows from compilation or synthetic model tests.

Bare-metal tests are required before broader CPU-performance conclusions. Memory
pressure and recovery experiments need separate protocols and cannot silently
be mixed into scheduler results.
