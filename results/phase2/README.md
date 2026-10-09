# Phase 2 evidence

These are immutable Linux measurements plus host-derived analysis. Final
scheduler/workload source: `109ce711bfad52c9f2ddfe2ba760b29258d58c25`.
The [results report](../../docs/PHASE2_RESULTS.md) explains the findings;
[benchmark commands](../../benchmarks/README.md) reproduce the experiments.

## Readable records

- [correctness.json](correctness.json): complete final Linux correctness suite.
- [correctness-metrics.json](correctness-metrics.json): independently extracted
  counts, service errors, finite fairness, concurrency and recovery timings.
- [map-footprint.json](map-footprint.json): actual loader-owned BPF map metadata.
- [environment.json](environment.json): final Linux/toolchain/config/BTF hashes,
  reference commits, VM parameters and 24 MHz architectural timer.
- [environment-review.json](environment-review.json): preemption selections,
  MONOTONIC/BOOTTIME bracket checks and original build/observer review.
- [benchmark-environment.json](benchmark-environment.json): final primary
  parameters, source hashes and actual binary fingerprints.
- [build-verifier.json](build-verifier.json): build provenance and accepted
  verifier summary counts. Full logs are in the diagnostic archive.
- [analysis.json](analysis.json), [analysis.md](analysis.md), [trials.csv](trials.csv):
  280 primary and 80 separate overhead/control trials, uncertainty, correlations,
  global/retained accuracy and observer contrasts.
- [cleanup.json](cleanup.json): default scheduler, BPF stats and preemption restored,
  swap removed and graceful VM shutdown.
- [local-review.json](local-review.json): final portable/sanitizer/documentation
  checks, unchanged remote main and existing main CI status. Phase 2 CI has not
  run because no push was made.
- [evidence-audit.json](evidence-audit.json): archive hashes/member/content audit
  and historical byte-preservation checks. Known secret signatures were checked;
  no scanner can exclude every unknown encoding or secret format.

## Raw archive inventory

[archive-inventory.json](raw/archive-inventory.json) records SHA256 and sizes for
22 gzip archives, totaling 314,139,184 compressed bytes. Each is below 100 MB.
There are 4,915 regular files and 549 directories, about 3.26 GB uncompressed.
Archives contain JSON/JSONL, text logs and CSV only; VM images, SSH keys,
compiled binaries, generated headers and source bundles are excluded.

| Archives | Contents and analysis status |
| --- | --- |
| `primary-r00` through `primary-r04` | All 280 final trial directories: raw workload/wake/scheduler events, perf/build output and per-trial summaries. Included. |
| `primary-manifest` | Final environment, randomized plan, commands, progress, summary and run log. Included. |
| `phase2-overhead-profile` | 40 arithmetic-timer/BPF-runtime trials. Separate study. |
| `phase2-observer-control` | 20 trials without wake tracing, with BPF accounting. Separate study. |
| `phase2-no-observers` | 20 trials without wake tracing or BPF accounting. Separate study. |
| `phase2a-final-validated` | Complete final full-preemption correctness run, including independent task output and loader-owned map inventories. Included. |
| `analysis-input` | Compact duplicate manifests/trial summaries and final correctness summary for convenient host analysis. |
| `aborted-r00` through `aborted-r02`, `aborted-manifest` | Earlier 149 successful trials at `8257281`, intentionally stopped after cold-counter race review. Preserved and excluded. |
| `phase2a-validated` | Earlier correctness run at `8257281`. Preserved; final report uses the full `109ce71` rerun. |
| `phase2-atomic-concurrency`, `phase2-atomic-observer-smoke` | Targeted atomic-counter/full-preemption and MONOTONIC smoke regressions before the final suite. Functional evidence only. |
| `phase2-bench-smoke3`, `phase2-special-smoke` | Seven-policy registration, compile and real memory-pressure smoke checks. Excluded from performance tables. |
| `validation-attempts` | Failed validation/setup attempts described below. Preserved and excluded. |
| `diagnostics` | Actual final/earlier build, verifier, suite and study logs, environment review and upstream build/load logs, including rejected lavd. |

All archive names above have `.tar.gz` suffixes. `validation-attempts` preserves:
late task-name opt-in exposing undercounted tracked telemetry; a threaded fixture
mistaking a real setup sleep for a false label; a map-ownership name collision;
an immediate map-release assertion that missed deferred cleanup; bpftrace 0.20.2
char-array syntax incompatibility; and acceptance of the version-suffixed
bpfland ops name. Fixes and successful reruns are recorded. None is silently
removed or treated as an optimization result.

## Verify and extract

From the repository root, this standard-library command verifies every archive
before extracting only the final analysis inputs. Extraction rejects unsafe
paths, links, special members and duplicate paths within each archive. Allow
roughly 2 GB of additional disk space for these selected inputs; extracting all
historical archives needs about 3.26 GB. Outputs go to ignored `build/`.

```sh
python3 - <<'PY'
import hashlib
import json
from pathlib import Path
import shutil
import tarfile

archives = Path('results/phase2/raw')
records = json.loads((archives / 'archive-inventory.json').read_text())
for record in records:
    source = archives / record['file']
    assert source.stat().st_size == record['bytes'], source
    digest = hashlib.sha256()
    with source.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    assert digest.hexdigest() == record['sha256'], source
selected = ['analysis-input', 'primary-manifest', 'phase2a-final-validated',
            'phase2-overhead-profile', 'phase2-observer-control',
            'phase2-no-observers'] + [f'primary-r{rep:02d}' for rep in range(5)]
output = Path('build/phase2-artifacts').resolve()
output.mkdir(parents=True, exist_ok=True)
for name in selected:
    seen = set()
    with tarfile.open(archives / (name + '.tar.gz'), 'r:gz') as archive:
        for member in archive:
            target = (output / member.name).resolve()
            assert target.is_relative_to(output), member.name
            assert member.isfile() or member.isdir(), member.name
            assert member.name not in seen, member.name
            seen.add(member.name)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as src, target.open('wb') as dst:
                    shutil.copyfileobj(src, dst)
print('Archive hashes verified; final inputs extracted.')
PY
python3 benchmarks/analyze_phase2.py \
  --primary build/phase2-artifacts/phase2-benchmark-final \
  --study profile=build/phase2-artifacts/phase2-overhead-profile \
  --study observer-control=build/phase2-artifacts/phase2-observer-control \
  --study no-observers=build/phase2-artifacts/phase2-no-observers \
  --output build/phase2-analysis
```

Python 3.9 or later is required for the extraction example. Analysis uses seeded
10,000-resample trial-level bootstrap estimates. Absolute input paths in the
JSON reflect the analysis machine; computed metrics, code hash and raw input
hashes permit reproducibility checks without requiring identical filesystem
paths. Primary contrasts are paired by randomized repetition block. Separate
observer sessions remain unpaired, with temporal-drift and observer-cost caveats.
Retained RMSE has 11 small completeness mismatches; global MAE avoids retention
loss but retains approximate nontransactional baseline boundaries. No earlier
implementation or smoke trial enters the final primary analysis.
