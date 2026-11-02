# Independent controller integration: ABI 2

[Lerwatt](https://github.com/Denny-Lin/lerwatt) and [Lertick](https://github.com/Denny-Lin/lertick) keep model development separate from their optional local demand/constraint interface. See the [shared integration guide](../../docs/INTEGRATION.md).

On **2026-11-02**, the candidate passed the [recorded checks](validation.json) on **Ubuntu 24.04.5 LTS ARM64**, kernel **7.0.0-38-generic**, **systemd 255**, in an eight-CPU QEMU VM. The scheduler, BPF maps and PID 1 service manager were real; CPU power controls were temporary simulated/file-based fixtures.

| Test | Observed result |
| --- | --- |
| Either startup order | Both workers became ready independently, then exchanged fresh ABI 2 data. |
| Lertick restart and online → EWMA policy selection | New attachment identity reconnected; Lerwatt's process remained unchanged. |
| Lerwatt restart and temporal → adaptive point prediction | Lertick's process/session remained unchanged; advice consumption resumed. |
| Paused demand publisher | Old demand expired; Lerwatt ran independently and reconnected after publication resumed. |
| Lerwatt stop | Journaled fixture settings restored; Lertick stayed attached and expired old advice. |
| Final shutdown | Default scheduling, unpinned maps and restored fixture controls were confirmed. |
| Actual installed Lertick helper | No-start install; persistent on/off; readiness; immutable binary; stop/update/rollback intent; uninstall all passed. |
| Actual installed Lerwatt helper | BPF-capable optional mode with absent peer; readiness; watchdog; sensor recovery; uninstall all passed, including a prefix containing spaces, `%` and `$`. |

The two repositories also run a frozen **56-value ABI check** in `make test-deployment`. It checks record layouts, constants, topology identity and validation semantics without importing either model. Run `python3 tests/check_integration_abi.py --peer /path/to/other/repo` to compare a candidate pair directly. Native Linux deployment and protocol fixtures passed on Ubuntu 24.04 ARM64 and Alpine 3.22 ARM64.

Reproduce the live tests from the Lertick checkout, only in an explicitly authorized idle disposable VM meeting each script's guards:

```sh
sudo python3 tests/test_power_integration.py --allow-vm-integration \
  --loader build/scx_lertick --lerwatt-repo ../lerwatt
sudo python3 tests/test_service_systemd.py --allow-systemd-test \
  --loader build/scx_lertick
```

From the Lerwatt checkout:

```sh
sudo python3 tests/test_service_systemd.py --allow-systemd-test \
  --daemon build/lerwattd --with-lertick --special-prefix
```

These are short lifecycle and interface tests. Selecting a different method does not demonstrate improved prediction, scheduling performance or energy savings. Physical-machine 24-hour/seven-day reliability and hardware energy/performance remain unvalidated. Future algorithm/kernel changes still require candidate verification; keeping ABI 2 does not make arbitrary implementation changes infallible.
