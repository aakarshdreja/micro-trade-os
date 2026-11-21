# QEMU results (emulation)

> **Source: QEMU emulation, not hardware.** Machine `olimex-stm32-h405`
> (emulated Cortex-M4), run on 2026-09-20 with QEMU 11.1.1.
> **Units are emulator time units, not CPU cycles.** Use these results for
> correctness and for the *direction* of each effect. For real timing, use
> [`../hardware_blackpill/`](../hardware_blackpill/).

The verbatim output of every QEMU run is recorded, with the commands used, in
[`rtos_kernel/docs/TEST_RECORD.md`](../../rtos_kernel/docs/TEST_RECORD.md). The
FreeRTOS comparison under QEMU is in
[`rtos_kernel/docs/COMPARISON_FREERTOS.md`](../../rtos_kernel/docs/COMPARISON_FREERTOS.md).

## Why these are not cycle counts

- **No cycle counter.** QEMU does not emulate the DWT counter, so the kernel
  falls back to a SysTick-based count.
- **Not cycle-accurate.** QEMU's virtual clock advances in coarse steps.
  Experiments 2 and 3 were run with `-icount shift=7`, which ties virtual time
  to the number of instructions executed. That makes relative comparisons
  meaningful, but it models no Flash wait states, no memory stalls and no bus
  contention.
- **Different clock model.** The QEMU build assumes an 8 MHz core, so its tick
  is 8,000 units long. The hardware runs at 100 MHz with a 100,000-cycle tick.

## What QEMU established

All of these passed under QEMU:

| Check | Result (QEMU) |
|---|---|
| All 5 firmware images build | 0 warnings, 0 errors at `-O2` |
| Bare-metal boot (`.data` copy, `.bss` zeroing, SysTick) | PASS |
| Demo pipeline schedulable (utilisation 0.160 vs bound 0.779) | PASS |
| Priority inheritance bounds the inversion | PASS |
| Queue correctness (byte integrity, FIFO order, full/empty edges), all queues incl. FreeRTOS | PASS |
| Lock-free path contains no interrupt-masking instructions (`objdump`) | PASS |

## QEMU figures (emulator units)

| Measure | FreeRTOS | Ours, tick-driven | Ours, poll mode / lock-free |
|---|---|---|---|
| Exp. 2: worst block | 29,568 | 31,160 | **3,828** |
| Exp. 2: jitter (max − min) | 26,407 | 27,806 | **1** |
| Exp. 2: fastest block | 3,161 | 3,354 | 3,827 |
| Exp. 3: 64-byte message, ping-pong | 9,290 | 9,245 (mutex queue) | **7,285** |
| Exp. 3: 64-byte message, burst | 9,445 | 9,277 (mutex queue) | **7,317** |
| Exp. 1: context switch | min 0 | min 0 | *inconclusive: below QEMU's resolution* |

QEMU ratios: poll mode 8.1× better than our tick-driven mode and 7.7× better
than FreeRTOS in the worst case; lock-free IPC 1.26× faster than our mutex queue
and 1.28× faster than FreeRTOS.
