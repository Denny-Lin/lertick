# Automatic independent peer discovery

Validation snapshots: Lerwatt base `cc8b65f`, Lertick base `5ce8a3c`, both on
`automatic-peer-discovery`. Each controller retains its own learning, controls
and lifecycle. No peer-specific installation flag is needed for new managed
installations. [Shared protocol and usage](../../docs/INTEGRATION.md).

## Implemented behavior

- New Lerwatt installations build the BPF-capable daemon and save
  `contract=auto`. No peer is installed or started by Lerwatt. Its own learned
  thermal model and MPC continue while waiting for a compatible running peer.
- New Lertick installations save `lerwatt=auto`. Normal service/foreground
  helper runs expose optional advice without an integration-enable command.
  Installing Lertick still does not start/enable scheduling; the normal local
  `lertick enable` command does that. Automatic mode preserves the previous
  governor-based CPUperf selection rather than forcing util just for discovery.
- Real maps plus fresh matching ABI 2, topology and attachment identities are
  required. File/package presence is not connection proof. Either startup order,
  peer stop and return, expired records and incompatible data retain independent
  operation. Neither project starts, stops or retrains its peer.
- Discovery never changes the selected predictor. New Lerwatt installs retain
  nominal independent MPC; cooperative prediction can be selected separately.
  Saved cooperative choices remain selected with or without Lertick. This
  supersedes the short-lived coupling between `--lertick` and prediction default.
  Existing saved settings, including explicit off opt-outs, survive upgrades.
- Automatic Lertick discovery preserves existing pins. If topology/identity or
  map publication is unavailable, it schedules independently. A writable BPF
  guard disables advice consumption before attach after publication failure,
  including a partial-publication case. The legacy strict `--lerwatt` diagnostic
  path retains its existing semantics; it is not the normal auto mode.
- An absent peer can join later without restart. A failed local transport setup
  (for example conflicting pins) remains unavailable until its environment is
  repaired and Lertick is locally restarted. Unknown pins are not force-cleaned.
  A compatible peer's appearance is not evidence of better forecast accuracy.

## Validation

[Linux build/model/deployment/e2e log](linux-build-and-tests.log) passed, including
both projects' required deployment and ABI checks. The peer's 56 compiled ABI
values match; the shared contract layout and integration guide are unchanged
between the two tested sources. [Source identity](source-identity.json) verifies
62 Lerwatt and 45 Lertick source/test/protocol files against the VM.

Real PID 1 service tests proved new defaults without explicit prediction/link
opt-in, absent-peer waiting, unlimited ready operation, recovery/restoration,
independent enablement intent, update/rollback and uninstall behavior. See
[initial lifecycle log](initial-live-pair.log) and
[final kernel/lifecycle log](status-fixture-failure.log); the latter's lifecycle
checks pass, but its subsequent obsolete status-label fixture fails as recorded
below. The final paired test is separately successful.

The [final live test](final-live-pair.log), [machine-readable result](result.json)
passed nine real sched_ext/BPF checks on Ubuntu 24.04.5 ARM64, kernel
`7.0.0-38-generic`, systemd 255 and eight vCPUs:

1. Lerwatt-first automatic connection, with actual BPF contract consumption.
2. Scheduler restart with a new session and automatic reconnection.
3. Changing scheduler online/EWMA mode leaves Lerwatt's PID running.
4. Frozen demand expires; resuming its publisher restores fresh exchange.
5. Lerwatt restart restores controls, expires advice and reconnects.
6. Changing Lerwatt's temporal/adaptive forecaster leaves Lertick's PID/session.
7. Lertick-first automatic connection.
8. Final control restoration, default-scheduler fallback and owned-pin cleanup.
9. A conflicting test-owned pin retains its map identity; automatic mode still
   registers an independent scheduler, reports off, and applies no contract.

All hardware controls were temporary simulator files. Kernel sched_ext/BPF and
PID 1 were real. Production services and physical CPU controls were not changed;
no VM reboot was performed. Final sched_ext state is disabled. Raw artifacts
remain retained in the VM and an ignored local archive; see [archive identity](raw-archive.json).

## Performance and retained failures

The [60 finite-work rows](finite-jobs.jsonl) reproduce earlier Linux outcomes
excluding CPU timing. All work completes, but the same two cooperative pairs
still regress in makespan by 0.02 and 0.07 seconds. The strict nonregression gate
is still failed, not waived: [summary](control-summary.json). No prediction
thresholds or learning parameters were tuned in this change. There is no new
hardware energy, throughput, model accuracy or overhead claim; default discovery
adds bookkeeping whose physical-machine cost still needs measurement.

Failures remain recorded:

- Old service/default and foreground-argv fixtures expected off/no-auto; the first
  runs failed. Their expectations now test the new behavior while retaining
  explicit off/on transaction coverage. [Service failure](first-service-fixture-failure.log),
  [argv failure](old-argv-fixture-failure.log),
  [fixed service](portable-service-fixtures.log),
  [fixed foreground](portable-foreground-fixtures.log).
- The first conflicting-pin test exposed the legacy preparation routine deleting
  the test-owned same-name pin. Auto mode now uses a separate non-destructive
  preparation path; it never invokes that stale-pin cleanup. Its test cleanup
  also handles an already absent owned directory. [Failed run](first-conflict-failure.log).
- After the production fix, the test waited for the wrong status prefix because
  its custom unit name lacked the scheduler classifier. The fixture name was
  corrected and a periodic runtime snapshot is now required before checking zero
  applied advice. [Failed fixture](status-fixture-failure.log),
  [final pass](final-live-pair.log).
- A local macOS plug-in compile attempt failed on Linux-only integer/header
  dependencies (`__u32`, `sys/statfs.h`), and is not counted as a pass. The Linux
  native plug-in, CPUperf and actual kernel tests pass. No platform-porting claim
  is made. The first two shell fixture sequences also continued to a documentation
  check after an earlier failure; those earlier failures are retained, not
  inferred successful from the enclosing shell exit code. Later validations use
  `set -e` or individually checked results.

Shared weights, joint retraining, physical energy savings, universal hardware
compatibility and long-duration reliability are not established by these tests.
