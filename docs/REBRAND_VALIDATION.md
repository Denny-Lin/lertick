# Lertick rebranding verification

Recorded on 2026-10-09. The owner approved publication and a CPU-only scope;
independent [naming research](NAMING.md) selected **Lertick** from twelve finalists.
Repository: [Denny-Lin/lertick](https://github.com/Denny-Lin/lertick).
Executable and sched_ext ops: `scx_lertick`.

## Preserved implementation and evidence

The original Phase 1 commit is **`1d2c39ecdd62add295287e8d7ad5fcd33335d01e`**.
It remains in history unchanged. All twelve files under `results/phase1/` were
compared byte-for-byte with that commit and match, including compressed logs,
raw measurements, commands, hashes, environment, and the original evidence README.

Ten implementation/build/test files were compared with the same commit after
normalizing only the declared branding substitutions. They match exactly,
except the explicit task-name predicate now recognizes the seven-character
`lertick` prefix instead of `sprout`. Workload names become `lertick_burst` and
`lertick_cpu`, both within Linux's task-name limit. No policy, arithmetic,
learning-rate, feature, callback-control-flow, CPU-accounting, slice-bound,
watchdog, or recovery change was introduced.

The continuing-task pending-service limitation remains documented and
unchanged. Phase 2 correctness or performance work has not started.

## Actual Linux regression

The existing disposable Ubuntu 24.04.5 ARM64 VM was reused with Linux
`7.0.0-38-generic`, QEMU 10.2.1/HVF, four vCPUs and 4 GiB RAM. Live BTF/config
hashes match the original experiment. Freshly generated BTF headers, BPF object,
skeleton, userspace loader, workload, and model test all compiled.
The [build transcript](../results/rebrand/build.log) contains the actual commands.

All **13 BPF programs** were accepted in the verbose online load; the largest
processed-instruction count was **5,301**. Fixed, shadow, and online modes each
passed verifier acceptance, registered `scx_lertick`, and executed real named
workloads. Original upstream helper/generated-skeleton warnings remain; no new
implementation warning or failed build was observed.

| Mode | Task runs | In-eBPF updates | Adaptive execution segments |
| --- | ---: | ---: | ---: |
| Fixed | 1,345 | 0 | 0 |
| Shadow | 1,360 | 1,346 | 0 |
| Online | 1,364 | 1,346 | 1,354 |

The runner verified one update per valid completed label, changed coefficients,
prediction-selected budgets reaching the kernel's running callback, and assigned
adaptive bounds of 0.5–5 ms. CPU-only competitors produced no completed training
labels. SIGINT, SIGTERM, SIGKILL, SysRq-S and actual stalled-task watchdog recovery
all passed, ending in `sched_ext/state=disabled`. Twelve short comparison runs
completed; their purpose is regression coverage, not an ML performance claim.

See [validation JSON](../results/rebrand/validation.json),
[exact commands](../results/rebrand/commands.txt),
[environment](../results/rebrand/environment.txt),
[tested source/binary hashes](../results/rebrand/source-and-binary-sha256.txt),
[verifier log](../results/rebrand/online-verifier.log.gz), and
[raw records](../results/rebrand/raw-evidence.tar.gz).

## Portable checks and publication controls

- Host and Linux model tests pass 200,121 independent arithmetic-oracle cases;
  host AddressSanitizer and UndefinedBehaviorSanitizer checks pass.
- Maintained documentation links, shell/Python syntax and Git whitespace checks
  pass. GitHub Actions performs these portable checks without privileged BPF.
- Tracked-file and Git-history credential checks cover private-key markers,
  common token formats, sensitive filenames, VM images, and nested evidence
  archives. VM disks, keys, guest console, and local build outputs stay ignored.
- Current source, binaries, CLI examples, Actions and project metadata use
  Lertick. Old identifiers remain only in preserved evidence, explicitly
  historical text, naming comparisons, and ignored local artifacts.
- The rebranding/scope commit is separate from the original Phase 1 commit.
  Publication targets `origin/main`; no release or tag is created.

The [main README](../README.md) and [Traditional Chinese README](../README.zh-TW.md)
describe the CPU-only experimental project. Memory-management findings are
retained in the [background archive](planning/README.md), neither active nor
backup development directions. No superiority over Linux EEVDF or another
sched_ext scheduler is claimed.
