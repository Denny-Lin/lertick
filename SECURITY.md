# Security Policy

## Status

This repository contains research documents and a local documentation checker. There is no released scheduler or supported production version. Future kernel experiments must begin in disposable VMs with a tested recovery path.

## Reporting

Use [GitHub private vulnerability reporting](https://github.com/Denny-Lin/linux-resource-learning/security/advisories/new) for a suspected security issue. Include affected commit, kernel/config if relevant, minimal reproduction, impact, and recovery observations. Remove secrets and unrelated personal data. Do not post a weaponized reproduction in a public issue before coordinated review.

The sole current maintainer is Denny-Lin. There is no guaranteed response SLA; a target of seven calendar days for initial acknowledgement is an aspiration. If private reporting is unavailable, consult the maintainer's [profile](https://github.com/Denny-Lin) for a private contact route and avoid posting sensitive details publicly.

## Future threat model

Review denial of service from unfair scheduling, map growth, overflow, verifier mistakes, untrusted task behavior, model poisoning, privileged-loader inputs, and telemetry leakage. The model must never bypass CPU affinity or kernel scheduling-class constraints. Watchdog fallback addresses some scheduler failures, not all performance or isolation failures.

No persistent system service should be installed automatically in early releases. Benchmark outputs should avoid command-line contents and raw process names unless the participant explicitly agrees. See the [architecture](docs/planning/ARCHITECTURE.md) and [deployment plan](docs/DEPLOYMENT.md).
