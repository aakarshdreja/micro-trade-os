# Results

The project was tested two ways. They answer different questions, and their
numbers are in different units, so they are kept apart.

| | **QEMU (emulation)** | **Hardware (WeAct Black Pill)** |
|---|---|---|
| Folder | [`qemu/`](qemu/) | [`hardware_blackpill/`](hardware_blackpill/) |
| What runs it | QEMU `olimex-stm32-h405`, an emulated Cortex-M4 on a laptop | A real STM32F411CEU6 chip, Cortex-M4 at 100 MHz |
| Timing source | SysTick-based count (QEMU has no cycle counter) | DWT cycle counter, exact to one CPU cycle |
| Units | **Emulator time units**, not cycles | **Real CPU cycles** (100 cycles = 1 µs) |
| Good for | Correctness: does it schedule, inherit priority, pass messages in order | Timing: how many cycles things actually cost, and the worst case |
| Detailed write-up | [`TEST_RECORD.md`](../rtos_kernel/docs/TEST_RECORD.md), [`COMPARISON_FREERTOS.md`](../rtos_kernel/docs/COMPARISON_FREERTOS.md) | [`HARDWARE_RESULTS.md`](../rtos_kernel/docs/HARDWARE_RESULTS.md) |
| **Cite in the report?** | Only for correctness | **Yes, for every timing figure** |

---

## Side by side

The same experiments, on QEMU and on the board. Compare the **ratios** across
the two columns, not the absolute numbers: the units differ.

### Experiment 2: execution jitter (worst block in a critical loop)

| | QEMU (units) | Hardware (cycles) |
|---|---|---|
| FreeRTOS, worst block | 29,568 | 100,431 |
| Ours, tick-driven, worst block | 31,160 | 100,577 |
| **Ours, poll mode, worst block** | **3,828** | **232** |
| Ours, poll mode, jitter (max − min) | **1** | **1** |
| **Poll mode vs FreeRTOS, worst case** | **7.7× better** | **433× better** |

### Experiment 3: 64-byte message passing (per message)

| | QEMU (units) | Hardware (cycles) |
|---|---|---|
| FreeRTOS queue | 9,290 | 667 |
| Our mutex queue | 9,245 | 493 |
| **Our lock-free ring** | **7,285** | **387** |
| Lock-free vs our mutex queue | 1.26× | 1.27× |
| **Lock-free vs FreeRTOS** | **1.28×** | **1.72×** |
| Cache-partitioned vs plain lock-free | 0.99× | 0.98× |

### Experiment 1: context switch

| | QEMU | Hardware (cycles) |
|---|---|---|
| FreeRTOS (task-selection step only) | min 0, inconclusive | 73–82 |
| Ours, full switch, MPU guard off | min 0, inconclusive | **57–65** |
| Ours, full switch, MPU guard on | n/a (MPU off in QEMU builds) | 84–95 |

### Other measurements

| | QEMU | Hardware (cycles) |
|---|---|---|
| Longest interrupts-disabled window | n/a (needs the cycle counter) | 168 (benchmark), 402 (demo) |
| Priority inheritance: high-priority task's wait | 64,000 units | 98 |

---

## Why the two sets of numbers differ

1. **Different units.** QEMU counts emulator time, not CPU cycles, so 7,285 on
   QEMU and 387 on the board describe the same operation in different units.
   Only the ratios between kernels are comparable.
2. **The poll-mode gain grows on hardware (7.7× → 433×).** In tick-driven mode
   the worst block is a normal block plus the time slice the competing task is
   given. On QEMU that interruption is about 28,000 units against a
   3,354-unit block, roughly 8× the block. On the chip it is about 100,000
   cycles (1 ms) against a 193-cycle block (under 2 µs), roughly 520× the
   block. Real silicon runs the loop far faster relative to the 1 ms tick than
   the emulator does, so the same interruption hurts far more, and removing it
   gains far more. The hardware figure is the real one.
3. **The lock-free gain over FreeRTOS grows on hardware (1.28× → 1.72×).**
   Under `-icount`, QEMU charges by instruction count and nothing for Flash
   wait states, memory stalls or bus contention. Real silicon charges for all
   of these, so the relative cost of two queue designs can shift between the
   emulator and the chip. The hardware ratio is the one to cite.
4. **Some things QEMU cannot measure at all.** Context-switch cost reads as 0
   because a switch is shorter than QEMU's clock step. The interrupts-disabled
   window needs the cycle counter. The MPU guard is only enabled in hardware
   builds.
5. **Short intervals are rounded up on QEMU.** The priority-inheritance wait
   reads 64,000 units on QEMU because QEMU's clock moves in tick-sized steps,
   while the board measures the real wait: 98 cycles.

**The conclusion is the same on both:** poll mode removes the jitter, lock-free
messaging is faster than a locked queue, and cache partitioning gives no speed-up
on a Cortex-M4. The hardware gives the real magnitudes.
