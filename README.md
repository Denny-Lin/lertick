<h1 align="center">Lertick</h1>

<p align="center">A Linux CPU scheduler that learns inside the kernel.</p>

<p align="center">
  <a href="https://github.com/Denny-Lin/lertick/actions/workflows/docs.yml"><img src="https://github.com/Denny-Lin/lertick/actions/workflows/docs.yml/badge.svg?branch=main" alt="CI status" /></a>
</p>

## Performance

Lertick was tested on 11 common scheduler benchmarks in an ARM64 virtual machine (Linux 7.0) with 4 and 8 CPUs. Compared with Linux's default scheduler (EEVDF), it is faster on `perf bench pipe`, `perf bench messaging` and hackbench, and has lower schbench wakeup latency. On 4 CPUs its schbench request latency with 4 message threads is 10% higher, as with the previous version. The rest are within 5% or better. It has not been tested on physical hardware yet.

<p align="center">
  <img src="docs/assets/lertick-scoreboard-cpu4.svg" alt="4 CPUs, 11 benchmarks. Against Linux EEVDF, Lertick is better on 7, the same on 3 and worse on 1. Against sched_ext schedulers, with an earlier Lertick version: scx_bpfland 9 better, 2 same, 0 worse; scx_lavd 8, 2 and 1; scx_p2dq 7, 2 and 2; scx_flash 8, 2 and 1." width="1120" />
</p>

<p align="center">
  <img src="docs/assets/lertick-scoreboard-cpu8.svg" alt="8 CPUs, 11 benchmarks. Against Linux EEVDF, Lertick is better on 8, the same on 3 and worse on 0. Against sched_ext schedulers, with an earlier Lertick version: scx_bpfland 8 better, 3 same, 0 worse; scx_lavd 4, 6 and 1; scx_p2dq 5, 6 and 0; scx_flash 8, 3 and 0." width="1120" />
</p>

<details>
<summary><strong>Each benchmark against Linux EEVDF</strong></summary>

<p align="center">
  <img src="docs/assets/lertick-vs-eevdf-cpu4.svg" alt="4 CPUs, medians of 6 runs: Lertick is better than Linux EEVDF on perf messaging processes and threads, perf pipe, hackbench threads and 16 groups, and schbench wakeup latency with 1 thread and with 4 message threads; within 5% on schbench 1-thread request latency, schbench throughput and kernel compile; worse on schbench request latency with 4 message threads." width="1120" />
</p>

<p align="center">
  <img src="docs/assets/lertick-vs-eevdf-cpu8.svg" alt="8 CPUs, medians of 6 runs: Lertick is better than Linux EEVDF on perf messaging processes and threads, perf pipe, hackbench threads and 16 groups, schbench wakeup latency with 1 thread and with 4 message threads, and kernel compile; within 5% on schbench request latency with 1 thread and with 4 message threads and schbench throughput; worse on none." width="1120" />
</p>

</details>

<sub>Benchmarks: perf bench sched messaging and pipe, hackbench, schbench and a rebuild of kernel/sched. They were also used to choose between design variants. Lertick ran with its default settings. [How it was measured](results/deadline-ordering/README.md#how-results-are-measured) · [All runs](results/deadline-ordering/RECORD.md#stealable-cpu-queues-2026-11-19)</sub>

## Get started

You need a Linux kernel with sched_ext: `cat /sys/kernel/sched_ext/state` should print `disabled`. Switching schedulers needs no reboot.

**1. Download**

```sh
git clone https://github.com/Denny-Lin/lertick.git
cd lertick
```

**2. Install** (Ubuntu 24.04)

```sh
sudo apt-get update
sudo apt-get install -y build-essential clang-18 llvm-18 pkg-config python3 \
  libelf-dev zlib1g-dev libzstd-dev linux-tools-common "linux-tools-$(uname -r)"
./deploy.sh install
```

**3. Turn on** (now and at every boot)

```sh
lertick enable
```

**4. Turn off** (Linux's default scheduler takes over immediately)

```sh
lertick disable
```

**5. Check status**

```sh
lertick status
```

**6. Everything else**

```sh
lertick help
```

**Remove**

```sh
lertick uninstall
```

If anything fails, Lertick stops and Linux's default scheduler keeps running.

[Documentation](docs/README.md) · [Other distributions](docs/INSTALL.md) · [Report a bug](https://github.com/Denny-Lin/lertick/issues/new?template=bug_report.yml) · License: GPL-2.0-only ([details](docs/legal/THIRD_PARTY.md))
