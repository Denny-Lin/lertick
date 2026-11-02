# Working on Lertick

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
