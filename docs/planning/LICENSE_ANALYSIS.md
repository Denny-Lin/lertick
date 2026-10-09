# License analysis

> Archived Phase 0 background research, superseded by Lertick's CPU-only scope.
> Original names (ScxSprout, scx-sprout, and linux-resource-learning), proposed
> phases, and statements below are preserved as historical reasoning. Memory
> allocation, reclaim, DAMON/DAMOS, NUMA memory placement, and joint CPU/memory
> management are background research only, neither active nor backup development
> directions. See the [archive index](README.md), [current Phase 1 results](../PHASE1_RESULTS.md),
> [decision record](../DECISIONS.md), and [current licensing](../../THIRD_PARTY.md).
> Phase 2 implementation has not been approved.

Reviewed: 2026-10-09. This records the project's licensing choice and engineering boundaries; it is not a legal opinion about every possible future integration.

## Current decision

**MIT** applies to current original documentation, templates, and the standard-library documentation checker. The [root license](../../LICENSE) is an OSI-approved permissive license. Its short notice keeps contribution and reuse straightforward; it is listed by the [Open Source Initiative](https://opensource.org/license/mit).

There is currently no BPF program, copied kernel implementation, or linked loader. A planned license split must be completed before such files are merged; the root license alone does not settle those obligations.

## Proposed implementation boundaries

| Component | Planned license | Reason and required review |
| --- | --- | --- |
| Original userspace loader/tooling | MIT | Simple reuse; audit linked library and generated-code obligations |
| BPF scheduler and derived kernel examples | GPL-2.0-only | Conservative fit with Linux examples, GPL-only kfuncs, and struct_ops |
| Shared model header compiled into both | MIT for entirely original code, or dual license if needed | Record per-file provenance; ensure GPL distribution obligations where combined |
| Copied scx/kernel helpers | Preserve upstream license | Do not replace SPDX identifiers or copyright notices |
| Generated BTF headers/skeletons | Review generator/input notices | Generated output is not automatically unencumbered |
| Benchmark dependencies/data | Their own licenses/consent | Record versions and redistribution terms; avoid bundling without review |

Future GPL files must carry `SPDX-License-Identifier: GPL-2.0-only`, retain notices, include the full applicable license text in `LICENSES/`, and declare a compatible BPF license section (normally `"GPL"`). That section is a loader-facing declaration, not a substitute for granting rights or distributing source. When deriving an upstream file, preserve its actual license rather than mechanically changing it to the proposed identifier.

## Kernel/BPF requirements

**Verified:** Linux checks compatibility when BPF uses GPL-only functions. Kernel documentation also describes GPL requirements for relevant struct_ops program types and kfunc calls. The kernel-facing implementation must audit the exact helpers and registration path on the pinned kernel; not every BPF program has identical requirements. [Linux BPF licensing](https://docs.kernel.org/bpf/bpf_licensing.html).

**Verified:** upstream `scx_simple.bpf.c` carries a GPL SPDX identifier and a `"GPL"` BPF license section. It is a useful provenance reference, not permission to relicense copied portions under MIT. [Upstream example](https://github.com/torvalds/linux/blob/v6.12/tools/sched_ext/scx_simple.bpf.c).

Separate userspace and kernel programs can carry different licenses; that does not automatically answer questions about generated skeletons, embedded binaries, linkage, distribution, or derivative code. Keep build artifacts and notices auditable. Obtain specialist review if a future packaging choice makes those boundaries uncertain.

## MIT versus Apache-2.0

MIT minimizes notice complexity and is the current choice. Apache-2.0 offers an express patent license and termination provisions, which may appeal to corporate contributors, but introduces additional terms and notice considerations. The Apache Software Foundation documents compatibility with GPLv3 and incompatibility with GPLv2; therefore an Apache-only policy across a Linux/GPLv2-derived codebase is unsuitable. [ASF compatibility discussion](https://www.apache.org/licenses/GPL-compatibility.html), [Apache license](https://www.apache.org/licenses/LICENSE-2.0).

An Apache userspace executable can still be a separate component in a mixed-license project, subject to the actual integration and dependency terms. This project's small initial scope does not need that extra licensing complexity. MIT is not selected to bypass kernel requirements.

## Contribution and release checks

Before the first code release: inventory every SPDX identifier and dependency; include license texts and notices; record copied-source SHAs; confirm libbpf and scheduler helper terms; ensure source accompanies GPL-covered distribution where required; make the README's license summary match the real files. CI should eventually validate SPDX/notice consistency, but no automated tool can establish legal compatibility on its own.

Original contributions use the applicable file license, with no CLA currently required. A maintainer cannot relicense someone else's contribution without permission. See [CONTRIBUTING](../../CONTRIBUTING.md).
