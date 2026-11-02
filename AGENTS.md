# Working on Lertick

Keep root Markdown limited to the product `README.md` and these agent
instructions. Use [docs/README.md](docs/README.md) as the documentation index;
guides and changelog belong in `docs/`, provenance in `docs/legal/`, and GitHub
community policies in `.github/`. Preserve root license files and update all
relative links when moving documents. Historical raw evidence retains its
original contents.

Keep deployment usable on the user's running Linux machine. Foreground runs are
continuous by default; finite trials must be explicit. Never attach a scheduler,
stop another experiment, change a kernel or run fault injection without an
explicitly authorized test environment. Do not use global SysRq as deployment
cleanup. Do not start or enable the service merely by installing files.

For deployment changes, run `make test-deployment` on Linux and
`python3 tests/check_docs.py`. Test actual attach/readiness/fallback in an idle,
authorized sched_ext VM or machine when changing loader lifecycle or service
transactions. Report the exact distro, architecture and running kernel; portable
fixture success is not proof of kernel attachment or hardware performance.

Preserve user service enablement and active state on update. Verify the staged
candidate before stopping the current service. Startup failure gets one attempt
to restore the previous complete bundle; never create an automatic restart loop.
Diagnostic reports are local and omit task names, PIDs and raw logs by default.

For scheduling or model improvements, state a measurable hypothesis first. Use
the existing benchmark tools, paired runs, a kernel-default baseline and the
previous release. Check throughput, tail latency and controller overhead as well
as prediction error. Keep raw evidence and adverse cases. Do not describe lower
prediction error as a scheduling speedup without workload evidence, change
historical results, or promise a universal performance gain. See
[the experiment overview](docs/OVERVIEW.md) and
[daily use and feedback](docs/DAILY_USE.md).

## Independent AI updates and Lerwatt

Lertick owns task scheduling, slices and CPU performance requests. Lerwatt owns
CPU power policy, CPUFreq min/max and PM QoS constraints. Neither controller
starts, stops, upgrades, trains or writes the private model state of its peer.
Keep the optional link independent of both service lifecycles.

The public interface is documented in [docs/INTEGRATION.md](docs/INTEGRATION.md).
Model/algorithm/application release versions are independent of contract ABI 2.
An ordinary model improvement must retain field layouts, meanings, units,
identity checks, timing/freshness bounds and control ownership. Do not bump the
ABI merely because a model changes. Inference/training must fit the cadence.

Run `make test-integration-abi` and `make test-deployment` on Linux for changes
affecting integration/deployment. When both checkouts are available, also run
`python3 tests/check_integration_abi.py --peer ../lerwatt`. Keep the vendored
contract header and shared integration guide synchronized. A deliberate schema
change needs an ABI migration and compatibility matrix, including safe mixed
versions; never silently reinterpret old records or weaken identity/freshness.

For integration lifecycle changes, use the explicit VM-only paired tests and
record the exact distro/architecture/kernel. Prove either startup order,
single-peer restarts, expiry, reconnection and independent operation. Preserve
each project's integration setting on update. Report configured and currently
connected states separately. See the existing deployment rules above before
attaching a scheduler or changing a service transaction.
