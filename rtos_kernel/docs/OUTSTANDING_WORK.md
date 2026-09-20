# Outstanding Work

What remains to be done. It is kept separate from the results documents, so
those report completed measurements only.

Everything below needs the physical STM32F4 Discovery board, or a human action.
None of it can be produced in software or under emulation.

---

## 1. Hardware measurement runs

QEMU has no DWT cycle counter and is not cycle accurate. It cannot produce the
timing figures the project's claims rest on. All three experiments must be
rebuilt at 168 MHz and re-run on the board:

```bash
make bench-ctxsw HW=1
make bench-jitter HW=1
make bench-ipc HW=1
make flash
```

| Experiment | What hardware adds | Priority |
|---|---|---|
| 1, context-switch cost | Everything. Emulation cannot measure this at all (it reports `min=0`). | **Highest** |
| 2, execution jitter | Exact cycle figures for a result whose shape is already established. | High |
| 3, 64-byte IPC | Real cycle costs in place of emulator time units; memory stalls and DMB drain now modelled. | High |

## 2. Logic-analyser capture

Probe PD12, the toggled signal, against PD13, which is held high across the
measured window. Capture both phases.

This gives an external view of the jitter claim. It can then be shown to agree
with the on-chip histogram.

## 3. Worst-case interrupts-disabled bound

Currently reported as `n/a (needs DWT; QEMU is not cycle-accurate)`. On
hardware the DWT counter makes this exact, producing the empirical basis for
the analytic interrupt-latency bound in the technical report.

## 4. MPU stack-overflow guard demonstration

Build with `make hw` (MPU guard enabled), deliberately overflow a task stack,
and confirm the CPU raises a MemManage fault instead of silently corrupting the
adjacent task's memory.

## 5. Figures for the report

Appendix E of the technical report lists the figures to capture from the
hardware runs, to expand the report toward its full length.

## 6. Publication and demonstration

- Push the repository to a remote (`git remote add origin <url>`, `git push -u origin main`).
- Live demonstration to the supervising faculty.

---

## Deliberately out of scope

**Zero-copy DMA ingest.** A natural continuation, and *not* claimed as
implemented. It cannot be verified under emulation. Calling it done on the
strength of a QEMU run would contradict the method this project argues for. It
is named here as future work.

**Porting to a cached Cortex-M7.** This is the platform where the cache-line
partitioning in `spsc_ca_t` would pay directly. It is therefore the sharpest
available test of that part of the design. On the Cortex-M4 it measures at
parity, which is expected for a part with no data cache.
