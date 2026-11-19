<h1 align="center">Lertick</h1>

<p align="center">A Linux CPU scheduler that learns inside the kernel.</p>

<p align="center">
  <a href="https://github.com/Denny-Lin/lertick/actions/workflows/docs.yml"><img src="https://github.com/Denny-Lin/lertick/actions/workflows/docs.yml/badge.svg?branch=main" alt="CI status" /></a>
</p>

## Performance

**Against Linux's default scheduler (EEVDF), Lertick is clearly faster on 8 of 11 standard kernel scheduler benchmarks on 4 CPUs and 4 on 8 CPUs, level on 2 and 6, and slower on 1.** The wins include `perf bench pipe`, hackbench and schbench wakeup latency, which it holds about 20% below EEVDF's. The loss is `perf bench messaging` with processes (6 to 8%). Run side by side with the community sched_ext schedulers, the previous build won far more than it lost: scx_bpfland 9 to 0 and 8 to 0, scx_lavd 8 to 1 and 4 to 1, scx_p2dq 7 to 2 and 5 to 0, scx_flash 8 to 1 and 8 to 0 (4 and 8 CPUs).

<p align="center">
  <img src="docs/assets/lertick-scoreboard-cpu4.svg" alt="4 CPUs, 11 benchmarks: against Linux EEVDF, Lertick is better on 8, the same on 2 and worse on 1; against scx_bpfland (previous Lertick build), better on 9, the same on 2 and worse on 0; against scx_lavd, better on 8, the same on 2 and worse on 1; against scx_p2dq, better on 7, the same on 2 and worse on 2; against scx_flash, better on 8, the same on 2 and worse on 1." width="1120" />
</p>

<p align="center">
  <img src="docs/assets/lertick-scoreboard-cpu8.svg" alt="8 CPUs, 11 benchmarks: against Linux EEVDF, Lertick is better on 4, the same on 6 and worse on 1; against scx_bpfland (previous Lertick build), better on 8, the same on 3 and worse on 0; against scx_lavd, better on 4, the same on 6 and worse on 1; against scx_p2dq, better on 5, the same on 6 and worse on 0; against scx_flash, better on 8, the same on 3 and worse on 0." width="1120" />
</p>

<details>
<summary><strong>Each benchmark against Linux EEVDF</strong></summary>

<p align="center">
  <img src="docs/assets/lertick-vs-eevdf-cpu4.svg" alt="4 CPUs, median of 2 sessions: Lertick is better than Linux EEVDF on perf messaging threads, perf pipe, hackbench threads and 16 groups, schbench 1-thread wakeup latency and schbench 4 message threads request latency, wakeup latency and throughput; the same on schbench 1-thread request latency and kernel compile; worse on perf messaging processes." width="1120" />
</p>

<p align="center">
  <img src="docs/assets/lertick-vs-eevdf-cpu8.svg" alt="8 CPUs, median of 2 sessions: Lertick is better than Linux EEVDF on perf pipe, hackbench threads, and schbench wakeup latency with 1 and with 4 message threads; the same on perf messaging threads, hackbench 16 groups, schbench request latency, schbench throughput and kernel compile; worse on perf messaging processes." width="1120" />
</p>

</details>

<sub>Benchmarks: perf bench sched messaging and pipe, hackbench, schbench and a kernel compile. They were first run as a held-out check; later rounds also used them to choose between variants. Lertick runs in its default learned (`online`) mode on one ARM64 VM (Linux 7.0) using 4 or 8 CPUs; "same" means within 5%. Against EEVDF: the current version (deadline order, 2026-11-18), median of 2 sessions of 8 runs each. Against the other schedulers: the previous build (without the deadline order), all six run in one session, shuffled, 8 runs; the deadline order mainly lowers Lertick's 4-CPU wakeup latency, so these counts are conservative. The kernel compile rebuilds kernel/sched. Physical hardware is untested. [All runs →](results/deadline-ordering/RECORD.md#deadline-order-2026-11-18)</sub>

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
