# Security Policy

## Status

Lertick contains an experimental C/eBPF sched_ext CPU scheduler and privileged loader. Native installation and finite foreground trials are available through the [installation guide](docs/INSTALL.md); there is no supported production release. Keep an independent administrator terminal or console for a new-kernel trial. The recorded runtime evidence comes from an Ubuntu ARM64 VM, not physical-hardware performance measurements. See the exact [deployment results](docs/PHASE1_RESULTS.md) and [VM reproduction](docs/DEPLOYMENT.md).

## Reporting

Use [GitHub private vulnerability reporting](https://github.com/Denny-Lin/lertick/security/advisories/new) for a suspected security issue. Include affected commit, kernel/config if relevant, minimal reproduction, impact, and recovery observations. Remove secrets and unrelated personal data. Do not post a weaponized reproduction in a public issue before coordinated review.

The sole current maintainer is Denny-Lin. There is no guaranteed response SLA; a target of seven calendar days for initial acknowledgement is an aspiration. If private reporting is unavailable, consult the maintainer's [profile](https://github.com/Denny-Lin) for a private contact route and avoid posting sensitive details publicly.

## Experimental threat model

Review denial of service from unfair scheduling, map growth, overflow, verifier mistakes, untrusted task behavior, model poisoning, privileged-loader inputs, and telemetry leakage. The model must never bypass CPU affinity or kernel scheduling-class constraints. Watchdog fallback addresses some scheduler failures, not all performance or isolation failures.

The loader has a finite lifetime and owns an unpinned struct_ops link. No persistent system service is installed. The deployment helper never invokes a global SysRq abort or stops a competing scheduler. A deliberate stalled-task test requires explicit fault flags, partial switching, an exact target TGID, and a detected QEMU/KVM VM; it is unavailable through the friendly helper. Watchdog and loader-death recovery passed on the recorded kernel; this does not guarantee recovery from arbitrary kernel failures.

The raw loader defaults to learning task names beginning with `lertick`, or an explicit TGID. The native helper explicitly enables `--track-all` for useful ordinary-application trials. Tracking controls learning eligibility, not which eligible fair-class tasks sched_ext schedules. Snapshots retain task names and numeric PIDs, but no command-line contents; requested telemetry files are newly created with mode `0600`. Existing files and special paths are refused. The retained snapshot map holds at most 4,096 entries; map-full failures are counted. Live per-task BPF storage follows kernel task lifetime. Review task churn, hostile workloads, fairness, and model poisoning before broader deployment.

Build from a checkout that other users cannot modify, and use a protected root-owned installation prefix. Builds and dependency downloads run as the normal user; only installation and kernel operations request sudo. A private runtime lock serializes Lertick ownership and maintenance. Other privileged sched_ext managers do not share that lock, and the kernel remains the final attachment authority. CPU cgroup quotas and hierarchical isolation are not implemented or validated by this scheduler; do not assume those policies retain their fair-scheduler semantics during a trial.
