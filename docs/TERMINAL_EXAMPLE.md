# Lertick terminal example

These are implemented commands. Run them inside the prepared Linux QEMU guest
after following the [deployment and build guide](../docs/DEPLOYMENT.md).
The actual tested environment on 2026-10-09 was Ubuntu 24.04.5 ARM64,
Linux `7.0.0-38-generic`, QEMU 10.2.1 with HVF hardware acceleration.

Start a finite online scheduler in one guest terminal:

```sh
cd /home/ubuntu/lertick
mkdir -p results/local
sudo build/scx_lertick --check
sudo build/scx_lertick --mode online --duration 15 --sample-ms 250 \
  --stats-json results/local/demo-scheduler.jsonl
```

While it runs, use another guest terminal to check registration and execute a
workload with a changing CPU target:

```sh
cd /home/ubuntu/lertick
cat /sys/kernel/sched_ext/state
cat /sys/kernel/sched_ext/root/ops
build/burst_workload --duration 6 --cpu 0 --burst-us 1000 --sleep-us 2000 \
  --phase-burst-us 6000 > results/local/demo-workload.jsonl
```

The live interfaces must report `enabled` and `scx_lertick`. Loader telemetry
reports completed/censored bursts, model coefficients, training updates,
predictions, adaptive decisions and slices observed by real execution callbacks.
Workload JSONL contains measured CPU service and wake/completion timing. These
measurements should be retained together rather than replaced by example output.

After the loader's deadline, check recovery:

```sh
cat /sys/kernel/sched_ext/state
```

It must report `disabled`. A normal SIGINT or SIGTERM also requests detach.
See [deployment recovery](../docs/DEPLOYMENT.md) for the independent SysRq and
VM-only watchdog tests; those are performed by the full validation runner.

The [recorded Phase 1 results](../docs/PHASE1_RESULTS.md) and
[validation evidence](../results/phase1/validation.json) contain actual outcomes.
The suite compares default Linux, official non-ML `scx_simple`, fixed and online
Lertick with identical workloads. No demonstration output or short VM comparison
establishes a performance improvement; recorded online p99 completion latency
was worse than fixed and default in the short Phase 1 comparison. Broader
experiments remain defined in the
[benchmark protocol](../docs/planning/BENCHMARK_PLAN.md).
