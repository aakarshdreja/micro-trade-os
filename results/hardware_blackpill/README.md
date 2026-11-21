# Hardware results (WeAct Black Pill)

> **Source: real hardware.** STM32F411CEU6 (Cortex-M4) at 100 MHz.
> **Units are real CPU cycles** (100 cycles = 1 µs), from the DWT cycle counter.
> These are the figures to cite. QEMU results are kept separately in
> [`../qemu/`](../qemu/); the two are compared side by side in
> [`../README.md`](../README.md).

Raw output of every hardware run, exactly as the board printed it over
semihosting. All runs: WeAct Black Pill (STM32F411CEU6, Cortex-M4) at 100 MHz
from the 25 MHz crystal, timed with the DWT cycle counter unless stated.

The analysis and the comparison with FreeRTOS are in
[`rtos_kernel/docs/HARDWARE_RESULTS.md`](../../rtos_kernel/docs/HARDWARE_RESULTS.md).

## Micro-Trade OS

| File | What it is | Headline |
|---|---|---|
| [`results_blackpill_demo.txt`](results_blackpill_demo.txt) | Full trading-pipeline demo: boot, schedulability analysis, priority-inversion test, latency reports | Context switch 90–98 cyc under load; interrupts-off window 402 cyc; PI blocking 98 cyc |
| [`results_blackpill_ctxsw.txt`](results_blackpill_ctxsw.txt) | Experiment 1: context-switch cost, MPU guard on | 84–95 cyc over 1,080,410 switches |
| [`results_blackpill_ctxsw_nompu.txt`](results_blackpill_ctxsw_nompu.txt) | Experiment 1: context-switch cost, MPU guard off | 57–65 cyc over 1,217,493 switches |
| [`results_blackpill_jitter.txt`](results_blackpill_jitter.txt) | Experiment 2: execution jitter, tick-driven vs poll mode | Worst block 100,577 → **232 cyc**; jitter 100,384 → **1 cyc** |
| [`results_blackpill_ipc.txt`](results_blackpill_ipc.txt) | Experiment 3: 64-byte IPC, mutex queue vs lock-free vs cache-partitioned | 493 / **387** / 391 cyc per message |

## FreeRTOS V11.1.0 baseline (same board, same clock)

FreeRTOS runs are timed with a SysTick-derived cycle count, which also counts
CPU cycles at 100 MHz.

| File | What it is | Headline |
|---|---|---|
| [`results_blackpill_frtos_ctxsw.txt`](results_blackpill_frtos_ctxsw.txt) | Experiment 1: context-switch cost (task-selection step only) | 73–82 cyc over 1,084,696 switches |
| [`results_blackpill_frtos_jitter.txt`](results_blackpill_frtos_jitter.txt) | Experiment 2: execution jitter | Worst block 100,431 cyc; jitter 100,242 cyc |
| [`results_blackpill_frtos_ipc.txt`](results_blackpill_frtos_ipc.txt) | Experiment 3: `xQueueSend` / `xQueueReceive`, 64-byte message | 667 cyc per message |

## Reading the files

- At 100 MHz, 100 cycles = 1 µs.
- Lines starting with `Info :` or `Open On-Chip Debugger` are OpenOCD's own
  output while flashing; the firmware's output starts at the `=====` banner.
- In the FreeRTOS files, `p50`/`p99` values are histogram bin edges; use
  `min`, `mean` and `max`, which are exact.

To reproduce, follow
[`rtos_kernel/docs/HARDWARE_TESTING_BLACKPILL.md`](../../rtos_kernel/docs/HARDWARE_TESTING_BLACKPILL.md);
each command there writes its output into this folder.
