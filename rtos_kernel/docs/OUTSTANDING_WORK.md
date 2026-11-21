# Outstanding Work

What remains to be done, and what has been deliberately removed from or left
out of scope. Kept separate from the results documents, so those report
completed measurements only.

---

## Completed since the previous revision

| Item | Status |
|---|---|
| Hardware measurement runs (all three experiments, cycle-accurate) | **Done** on a WeAct Black Pill (STM32F411CEU6, Cortex-M4, 100 MHz), using the DWT cycle counter. See [`HARDWARE_RESULTS.md`](HARDWARE_RESULTS.md). |
| FreeRTOS baseline on hardware | **Done**, same board, same clock, same three experiments |
| Worst-case interrupts-disabled bound | **Done**: 168 cycles (context-switch benchmark), 402 cycles (full demo workload) |
| Publication | **Done**: repository is on GitHub |

The hardware runs used the Black Pill rather than the STM32F4 Discovery. Both
are Cortex-M4 STM32F4 parts with an MPU and a DWT cycle counter, so cycle
counts carry over; only microsecond figures depend on the clock (100 MHz here,
168 MHz on the Discovery).

---

## Removed from scope

**Logic-analyser capture of the jitter waveform.** Originally planned as a
second, external view of Experiment 2. It has been removed. The jitter result
rests on the on-chip DWT cycle counter, which times every one of the 4,000
blocks per phase directly; that measurement is complete and is reported in
[`HARDWARE_RESULTS.md`](HARDWARE_RESULTS.md). The GPIO toggling remains in the
benchmark as its workload.

---

## Still outstanding

### 1. MPU stack-overflow guard demonstration

The guard is enabled in every hardware build and runs on every context switch
(about 27 cycles per switch, measured). What is not yet shown is a deliberate
overflow: build a task that exceeds its stack, and confirm the CPU raises a
MemManage fault instead of silently corrupting the adjacent task's memory.

### 2. Live demonstration

Run the demo and benchmarks on the board in front of the examiners
(`make hw BOARD=blackpill && make run-hw`).

---

## Deliberately out of scope

**Zero-copy DMA ingest.** A natural continuation, and *not* claimed as
implemented. It is named here as future work.

**Porting to a cached Cortex-M7.** This is the platform where the cache-line
partitioning in `spsc_ca_t` would pay directly. It is therefore the sharpest
available test of that part of the design. On the Cortex-M4 it measures at
parity (0.98-0.99x on hardware), which is expected for a part with no data
cache.
