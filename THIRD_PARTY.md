# Third-party provenance and licensing

The Lertick scheduler and loader are derived from the official
[sched-ext/scx-c-examples](https://github.com/sched-ext/scx-c-examples) C examples,
pinned at commit `370b1cea95c6bb668d5b9261f069d433142b9986`.
`scx_simple.c` and `scx_simple.bpf.c` supply the struct_ops loading, weighted
virtual-time dispatch, and user-exit-info patterns; `scx_qmap.bpf.c` supplies the
task-storage allocation pattern. The implementation files retain attribution and are
licensed under GPL-2.0-only, with the full text in
[LICENSES/GPL-2.0-only.txt](LICENSES/GPL-2.0-only.txt).

`vendor/scx/` contains only the common, compatibility, enum, and user-exit-info
headers needed by these two implementation files. `vendor/bpf-compat/gnu/stubs.h`
is the official empty glibc stub shim needed for cross-target BPF compilation.
These files are copied unchanged from the pinned revision, including their
original notices. Generated enum headers belong to that same revision; they are
not generated from the deployment guest. `scripts/fetch_reference.sh` retrieves
the complete pinned source for inspection and comparison; the build itself does
not require a network fetch.

The project's original research documents and independently authored material
retain the root MIT license. That license does not override the GPL notices in
the scheduler, loader, their build scripts, or vendored helpers. Distributed
scheduler executables and BPF objects require corresponding GPL source and
notices.

`build/vmlinux.h` is generated locally from the **running Linux guest's**
`/sys/kernel/btf/vmlinux`; it is ignored by Git and must not be substituted by a
host/container header. libbpf, LLVM/Clang, bpftool, Linux, and QEMU are external
build/runtime dependencies supplied by their distributions and upstream licenses;
their binaries are not vendored here. The Ubuntu deployment guest uses
bpftool 7.7 with libbpf 1.7.0; use matching/new enough libbpf development headers
for the skeleton emitted by bpftool. Ubuntu's libbpf 1.3.0 headers lack the
`bpf_map_skeleton.link` field emitted by this bpftool. An isolated libbpf 1.7.0
prefix can be selected with `PKG_CONFIG_PATH` without replacing system libbpf.

Phase 1 was originally validated under the ScxSprout name. Historical research
and raw validation evidence retain that name and the former repository path;
renaming does not change their attribution, licenses, or recorded measurements.
