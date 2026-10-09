# Security Policy

## Status

Lertick contains an experimental C/eBPF sched_ext CPU scheduler and privileged loader. Phase 1 was validated in a disposable Ubuntu QEMU VM; there is no supported production release. Run new kernel experiments in disposable VMs with a tested recovery path. See the exact [deployment results](docs/PHASE1_RESULTS.md) and [commands](docs/DEPLOYMENT.md).

## Reporting

Use [GitHub private vulnerability reporting](https://github.com/Denny-Lin/lertick/security/advisories/new) for a suspected security issue. Include affected commit, kernel/config if relevant, minimal reproduction, impact, and recovery observations. Remove secrets and unrelated personal data. Do not post a weaponized reproduction in a public issue before coordinated review.

The sole current maintainer is Denny-Lin. There is no guaranteed response SLA; a target of seven calendar days for initial acknowledgement is an aspiration. If private reporting is unavailable, consult the maintainer's [profile](https://github.com/Denny-Lin) for a private contact route and avoid posting sensitive details publicly.

## Experimental threat model

Review denial of service from unfair scheduling, map growth, overflow, verifier mistakes, untrusted task behavior, model poisoning, privileged-loader inputs, and telemetry leakage. The model must never bypass CPU affinity or kernel scheduling-class constraints. Watchdog fallback addresses some scheduler failures, not all performance or isolation failures.

The loader has a finite lifetime and owns an unpinned struct_ops link. No persistent system service is installed. A deliberate stalled-task test requires explicit fault flags, partial switching, an exact target TGID, and a detected QEMU/KVM VM. Watchdog and loader-death recovery passed on the recorded kernel; this does not guarantee recovery from arbitrary kernel failures.

Learning and telemetry select task names beginning with `lertick`, or an explicit TGID. Snapshots retain task names and numeric PIDs, but no command-line contents. The retained snapshot map holds at most 4,096 entries; map-full failures are counted. Live per-task BPF storage follows kernel task lifetime. Review task churn, hostile workloads, fairness, and model poisoning before broader deployment.
