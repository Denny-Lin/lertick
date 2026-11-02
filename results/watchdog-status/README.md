# Watchdog exit classification and optional plug-in integration

Linux 7.0.0-38-generic, aarch64 QEMU/HVF, eight vCPUs. The tested scheduler
source is `ed04d6e`; [provenance](provenance.json) and the integration environment
record source and binary hashes. This change clarifies exit telemetry. It does
**not** fix or explain the natural stall in the
[model confirmation](../model-confirmation/README.md).

## Exit classification

The existing `watchdog_observed` field only identifies an **expected** watchdog.
It could therefore be false on a real unexpected runnable-task stall. Preserve
that legacy field and all exit-status decisions; add `watchdog_expected` and
`watchdog_triggered` so consumers can distinguish the cases.

Actual [VM results](lertick-watchdog-flags-ed04d6e/validation.json):

| Case | Expected | Triggered | Legacy observed | Unexpected fallback | Loader status |
| --- | --- | --- | --- | --- | --- |
| Normal shutdown | false | false | false | false | 0 |
| Intentional stall with expectation flag | true | true | true | false | 0 |
| Intentional stall without expectation flag | false | true | false | true | 1 |

All three test cases passed and restored the default Linux scheduler. The last
loader's status 1 is the required result, not a successful loader exit. The
fault tests use the existing guarded partial-switch mechanism in a disposable
VM. Run `sudo python3 tests/validate_watchdog_flags.py --output FRESH_DIRECTORY`
after building the scheduler and workload in an isolated QEMU/KVM Linux guest.

If kernel BTF lacks the stall enum, `watchdog_triggered` is `null`; normal runs
remain allowed and an explicitly expected watchdog still requires the enum.
That compatibility branch was reviewed but not exercised on this kernel.
The BPF program, model, slices, defaults and fallback gate are unchanged.
Compilation succeeded; existing signedness warnings in vendored `compat.h`
are retained in [the build log](build-and-first-integration.log).

## Eight-vCPU integration

The complete [fresh rerun](lertick-temporal-integration-ed04d6e-decoded/summary.json)
used the real scheduler and Lerwatt daemon with the RK3588 file-interface
simulator. All eight pinned workers alternated 30% and 90% duty cycles.
The scheduler ran 240 seconds with `--lerwatt --track-all --model-init ewma
--model-update carry`; the daemon ran 250 seconds with `--policy mpc --forecast
temporal --cross-cluster`. Both standalone checks before and after used the
ordinary scheduler defaults without Lerwatt.

| Check | Recorded result |
| --- | --- |
| Cluster CPU sets | 0–3, 4–5, 6–7 |
| Maximum observed CPU utilization | 1022–1024 out of 1024 on every CPU |
| All three clusters simultaneously above 50% busy | 1,470 telemetry rows |
| Rows with joint planning active | 1,595 |
| Cluster forecasts selecting temporal prediction | 152 |
| Contract applications | 1,545,310 |
| Contract/demand map | Three consistent version-1, nonzero even-sequence entries each |
| Simulator peak temperatures, big0 / big1 / little | 49.9 / 49.9 / 48.1 °C |
| Throttle steps / bad writes / daemon write errors | 0 / 0 / 0 |
| Actuator restoration | All 30 integer knob values restored; journal removed |
| Scheduler cleanup | Default Linux restored; both pinned maps removed |
| Standalone before and after | Both passed |

These are activation and cleanup observations from one simulated board, not
hardware validation or evidence of improved prediction accuracy, throughput,
energy efficiency or controller superiority. The default control policy stays
`rule`; scheduler defaults stay `bias` / `trunc`.

## Failed attempt and retained evidence

The [first integration attempt](lertick-temporal-integration-ed04d6e/three-cluster/summary.json)
remains `completed: false`. At the map audit, the collector treated bpftool's
raw byte-array key as an integer and raised `TypeError: unhashable type: 'list'`.
The [actual dump](lertick-temporal-integration-ed04d6e/three-cluster/lw_contract-0.json)
also contains the decoded integer/dictionary fields under `formatted`.
All three programs exited 0, all 30 knob values were restored and the journal
was removed; the after-standalone check was not reached on that attempt.

Only the test decoder changed: use `formatted` when present and validate the
semantic key/value types. The actual nested representation and the older
direct representation both passed local decoder fixtures. The full 250-second
integration then restarted from a fresh output directory and passed, including
both standalone checks. Binary hashes match across attempts. No model,
parameters, temperature limits or runtime gates changed to obtain the pass.
Both [original](verify-new-model-plugin-key-array-failure.py) and
[corrected](verify-new-model-plugin.py) collectors are preserved. Their paths
target this disposable VM; reproduction elsewhere requires adjusting them.

The complete 20,272,308-byte raw archive is retained locally in
`results/local/watchdog-status-ed04d6e/lertick-watchdog-integration-ed04d6e.tgz`
in the main checkout and validation worktree. It includes all three output
roots, not just the passing rerun. [The archive manifest](archive.json) records
SHA-256 `84bed19679efb78dab7c79b787a1318f625d441cb021e212d632696bc4426b1b`.
Small summaries and the build/failure log are committed here; the full raw
archive is not uploaded to Git.
