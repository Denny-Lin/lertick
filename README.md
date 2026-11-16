<h1 align="center">Lertick</h1>

<p align="center">A Linux CPU scheduler that learns inside the kernel.</p>

<p align="center">
  <a href="https://github.com/Denny-Lin/lertick/actions/workflows/docs.yml"><img src="https://github.com/Denny-Lin/lertick/actions/workflows/docs.yml/badge.svg?branch=main" alt="CI status" /></a>
</p>

## Performance

**Lertick beats Linux's default scheduler (EEVDF) on 5 of 11 standard kernel scheduler benchmarks with 4 CPUs and on 6 with 8 CPUs, and the community scheduler scx_bpfland on 6 and 9.** It loses to EEVDF on 2 and 3, mostly when many message threads wake groups of workers.

<p align="center">
  <img src="docs/assets/lertick-scoreboard-cpu4.svg" alt="4 CPUs, 11 benchmarks: against Linux EEVDF, Lertick is better on 5, the same on 4 and worse on 2; against scx_bpfland, better on 6, the same on 4 and worse on 1." width="1120" />
</p>

<p align="center">
  <img src="docs/assets/lertick-scoreboard-cpu8.svg" alt="8 CPUs, 11 benchmarks: against Linux EEVDF, Lertick is better on 6, the same on 2 and worse on 3; against scx_bpfland, better on 9, the same on 2 and worse on 0." width="1120" />
</p>

<details>
<summary><strong>Each benchmark against Linux EEVDF</strong></summary>

<p align="center">
  <img src="docs/assets/lertick-vs-eevdf-cpu4.svg" alt="4 CPUs: Lertick is better than Linux EEVDF on perf pipe, hackbench threads, schbench 1-thread request and wakeup latency, and kernel compile; the same on perf messaging, hackbench 16 groups and schbench 4-thread throughput; worse on schbench 4 message threads request and wakeup latency." width="1120" />
</p>

<p align="center">
  <img src="docs/assets/lertick-vs-eevdf-cpu8.svg" alt="8 CPUs: Lertick is better than Linux EEVDF on perf pipe, hackbench threads and 16 groups, schbench 1-thread request and wakeup latency, and schbench 4-thread throughput; the same on schbench 4-thread request latency and kernel compile; worse on perf messaging processes and threads and schbench 4 message threads wakeup latency." width="1120" />
</p>

</details>

<sub>Benchmarks: perf bench sched messaging and pipe, hackbench, schbench and a kernel compile, none of them used to choose Lertick's settings. Lertick runs in its default learned (`online`) mode. Median of 5 runs on one ARM64 VM (Linux 7.0) using 4 or 8 CPUs; "same" means within 5%. Physical hardware is untested. [All runs →](results/deadline-ordering/README.md#held-out-result-on-8-cpus-2026-11-16)</sub>

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

[Documentation](docs/README.md) · [Other distributions](docs/INSTALL.md) · [Report a bug](https://github.com/Denny-Lin/lertick/issues/new?template=bug_report.yml) · License: GPL-2.0-only scheduler, MIT model and docs ([details](docs/legal/THIRD_PARTY.md))
