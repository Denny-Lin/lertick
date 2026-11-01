# Lerwatt startup and cluster validation

## Scope

Validation uses the unchanged scheduler from `cc66050` (PR #1), Ubuntu ARM64
kernel `7.0.0-38-generic`, and Lerwatt `39893f3`. The scheduler changes in this
branch are limited to the VM launcher; no BPF or loader behavior is changed.

## Why `contract_stale` stopped at 819

The original `/tmp/pr1-v1b/scx.log` contains these cumulative counters:

| Seconds from first snapshot | contract_stale | contract_applied |
| --- | ---: | ---: |
| 0.000 | 12 | 0 |
| 10.037 | 819 | 7798 |
| 20.049 | 819 | 16842 |
| 60.063 | 819 | 53752 |

The ten-second sampling interval does not establish ten seconds of stale
records. `contract_stale` counts failed record reads, not elapsed seconds or
control periods. `contract_get()` also counts an uninitialized record (`seq ==
0`), an in-progress/inconsistent publication, an incompatible version, or an
expired record as stale. It falls back to neutral contract values.

The loader pins newly allocated maps before attaching the scheduler. A daemon
started earlier initially sees no maps and retries every second
(`CONTRACT_RETRY_NS` in Lerwatt). Until its first publication, the new contract
records are zero-filled and reads count as stale. At the default 100 ms
control period, publications remain valid for 300 ms. Once the daemon attaches
and publishes regularly, applied reads grow while the cumulative stale count
remains unchanged. The original daemon log confirms `absent` followed by
`attached`.

Three controlled runs used four busy workers, one pinned to each of the four
vCPUs, and 200 ms scheduler snapshots over twelve seconds:

| Startup order | First snapshot with applied reads | Final stale | Final applied |
| --- | ---: | ---: | ---: |
| Daemon first, scheduler about 0.2 s later | 0.612 s | 2443 | 59965 |
| Scheduler first, daemon about 0.2 s later | 0.408 s | 1124 | 60571 |
| Scheduler without daemon | none | 60559 | 0 |

In both attached runs the stale count was constant after three seconds while
applied reads continued growing. Without a daemon, stale reads continued
throughout the run. The exact counter depends on workload and startup timing;
819 is not a threshold or saturation value. Starting the daemon first explains
the retry delay, but either startup order necessarily has a window before the
first valid publication. No scheduler fix is warranted by these results.

Full logs and per-case summaries are retained in the VM under
`/var/tmp/lerwatt-verification-mhqa6lvj/`.

## Standalone regression

`make test-model test-accounting test-cpuperf test-lerwatt` passed in the VM.
A three-second run without `--lerwatt` exited with status zero, no unexpected
fallback, and `default_restored: true`; sched_ext returned to `disabled`.

## Eight-vCPU launch

The VM launcher retains four vCPUs by default and accepts an explicit count:

```sh
python3 scripts/qemu_vm.py --accelerator hvf --cpus 8 start
```

Memory remains 4096 MiB and the existing disk overlay is reused. Before the
restart, the two legacy simulator trees from `/run` were archived together in
`/var/tmp/lerwatt-run-before-8cpu.tgz`. A CPU-map-only change would remap the
simulator's workload input, while the current scheduler still assigns host
CPU IDs from the simulated cpufreq policies; eight vCPUs preserve matching
CPU IDs on both sides of the contract.

The restarted guest reported eight CPUs and the same kernel. A 25-second
`--lerwatt --track-all` scheduler run used eight pinned busy workers, a
`procstat` simulator, bare-board cooling, 25 C ambient, a 79 C initial die
temperature, and the default rule controller with its unchanged 80 C soft
limit. Both directions of the contract were inspected about eight seconds
after scheduler registration.

| Cluster | Host CPUs | Maximum busy fraction | Contract ceiling cap | Demand cpuperf max | Demand predicted utilization |
| --- | --- | ---: | ---: | ---: | ---: |
| little | 0-3 | 1.000 | 312 | 312 | 395 |
| big0 | 4-5 | 1.000 | 604 | 604 | 1021 |
| big1 | 6-7 | 1.000 | 604 | 604 | 1021 |

All three clusters were simultaneously busy in controller telemetry. Every
CPU reached scheduler utilization 1023. All three contract records had
version 1 and even sequence 154; the demand records had version 1 and even
sequence 160. Controller energy prices were 1000 at the inspection point.
The frequency ceilings moved from 1800/2400/2400 MHz down to
1416/1416/1200 MHz over the run (little/big0/big1).

The stale count stopped at 3153 before the first snapshot with applied reads
at 0.404 s, and remained constant thereafter; final applied reads were
221162. The controller reported thirteen knob writes and zero write errors;
the simulator reported zero bad writes. Scheduler exit status was zero,
`unexpected_fallback` was false, `default_restored` was true, maps were
unpinned, and sched_ext returned to `disabled`.

**Thermal limitation:** simulator tick-level peaks were 85.9/85.0/83.2 C
(big0/big1/little), with seven kernel thermal throttle steps during this hot
start. Thus this run verifies contract integration under load; it does not
demonstrate a one-degree safety margin below the 85 C passive trip. The
100 ms controller telemetry alone underreported the big0 peak as 85.0 C.
This short integration run is not an MPC comparison or steady-state
performance claim. The default policy and thermal limits are unchanged.

Raw logs, map dumps and summaries remain in
`/var/tmp/lerwatt-verification-px2h5124/eight-cpu-hot/`. The temporary Python
harness is `/var/tmp/verify_lerwatt_vm.py`; its summary label initially used
the last map name because of a loop-variable collision. Only that metadata
label was corrected to the directory name; measured values and raw logs
were preserved.

Afterward, two consecutive virtual-time runs using the exact default
`/run/lerwatt-sim` path succeeded without `--force`; only its persistent
identity remained, with no live marker. A separate three-second standalone
Lertick run on eight vCPUs also exited normally and restored the default
scheduler. The VM remains running with eight vCPUs and no active simulator,
controller or sched_ext scheduler.
