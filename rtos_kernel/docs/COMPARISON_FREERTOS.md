# Head to Head: This Kernel vs FreeRTOS

The project objective is stated against FreeRTOS. So FreeRTOS was built and
measured, not just cited. This document is the comparison.

The baseline is **FreeRTOS V11.1.0**, ARM_CM3 port. It lives in
[`../../freertos_baseline/`](../../freertos_baseline/).

## Experimental control

The kernel is the only thing that changes. Everything else is identical in both
images:

| Held constant | How |
|---|---|
| Boot layer | `startup_stm32f4.s` and `linker.ld`, copied byte for byte |
| Measurement code | the same `stats.c`, so the same histogram and percentile engine |
| Cycle clock | the same SysTick formula. QEMU has no DWT on either side |
| GPIO driver | the same `gpio.c`, same single-store BSRR toggle |
| Compiler and flags | `arm-none-eabi-gcc` 15.2.1, `-O2`, soft float |
| Machine | `olimex-stm32-h405`, `-icount shift=7` |
| Workload | same block size, message size, access patterns, sample counts |

FreeRTOS is configured to match ours, not to be handicapped. It runs at 8 MHz
with a 1 kHz tick. It is preemptive with time slicing. It has 8 priority levels.
`configUSE_PORT_OPTIMISED_TASK_SELECTION` is on, which is the same CLZ-based
selection our scheduler uses. Priority-inheritance mutexes are on. Trace and
runtime-stats facilities are off, because our kernel does not pay for them
either.

ARM_CM3 rather than ARM_CM4F: it targets Cortex-M without the FPU, matching our
soft-float build. ARM_CM4F would save FPU context on every switch.
Our kernel does not save it. That would be a difference in work done, not a
difference in kernel design.

**About the units.** QEMU is not cycle accurate and has no DWT counter. Under
`-icount`, virtual time tracks instruction count. So the numbers below are
emulator time units, not cycles. Both kernels use the same clock, so the
**ratios** can be compared. The absolute numbers must come from hardware.

---

## 1. Execution jitter

This is the headline result.

The test runs 4000 blocks of 32 GPIO toggles. A partner task at the same
priority runs throughout, so the scheduler really does time-slice the task under
test.

| | FreeRTOS | Ours, tick-driven | Ours, poll mode |
|---|---|---|---|
| uninterrupted block (min) | **3,161** | 3,354 | 3,827 |
| worst observed block (max) | 29,568 | 31,160 | **3,828** |
| peak-to-peak jitter | 26,407 | 27,806 | **1** |
| distribution | multi-modal, 4 bins | multi-modal, 5 bins | **single bin, 4000/4000** |

**Against FreeRTOS, poll mode cuts the worst-case block from 29,568 to 3,828.
That is a 7.7x improvement. Peak-to-peak jitter drops from 26,407 units to 1.**

**FreeRTOS is slightly faster on the uninterrupted path**: 3,161 units against
our 3,354, about 6%. This kernel is not faster at raw execution. The claim is
determinism.

**Our tick-driven mode matches FreeRTOS**: 27,806 against 26,407 peak-to-peak,
31,160 against 29,568 worst case. This makes the Phase A baseline in our own
testbed an honest stand-in for a real RTOS, so the Phase A to Phase B
improvement is not an artefact of a weak in-house baseline.

The gap on the minimum is 3,827 against 3,354. That 473-unit difference is the
cost of the poll machinery itself. It is what buys the collapse in the other two
rows.

FreeRTOS has **no way to suppress its scheduler tick** for a critical section.
There is no Phase B on that side. That absence is the point of the comparison.
It is a capability difference, not a tuning difference.

---

## 2. 64-byte IPC latency

The test runs 200 batches of 256 round trips, in two access patterns.

Figures are per message. The distributions are very tight, with a spread of 3
units or less over 200 samples. So the exact minimum is quoted rather than a
bin-quantised percentile.

| Design | Ping-pong (depth 1) | Burst (depth 8) |
|---|---|---|
| **FreeRTOS** `xQueueSend` / `xQueueReceive` | 9,290 | 9,445 |
| Our mutex-queue baseline | 9,245 | 9,277 |
| **Our lock-free SPSC** | **7,285** | **7,317** |
| Our cache-partitioned SPSC | 7,340 | 7,329 |

**Against FreeRTOS, our lock-free queue is 1.28x faster on ping-pong and 1.29x
faster on burst.** Both sides passed their correctness gates: round-trip byte
integrity, FIFO ordering, full and empty edges.

**Our in-house baseline is within 0.5% of real FreeRTOS**: 9,245 against 9,290.
That is the measured justification for using it as a proxy.

**Cache-line partitioning measures at parity** with the plain lock-free ring:
7,340 against 7,285. Expected on this part. The Cortex-M4 has no data cache and
single-cycle SRAM, so the load the shadow indices remove costs about as much as
the branch and store that replace it. The partitioning is a portability and
multi-core property, not a Cortex-M4 speed-up.

### The structural difference

Per 64-byte message:

| | Critical sections entered | Can extend another task's worst-case latency? |
|---|---|---|
| FreeRTOS `xQueueSend` / `xQueueReceive` | 4 | **Yes** |
| Our mutex-queue baseline | 4 | **Yes** |
| Our lock-free SPSC | **0** | **No** |

This is verified in the disassembly, not inferred:

```bash
arm-none-eabi-objdump -d build/spsc.o | grep -c cpsid    # -> 0
```

A lock masks interrupts. While it does, every other task in the system has its
worst-case latency extended. That includes tasks that never touch the queue. A
wait-free ring cannot do this. Not at any load, and not under any contention.

This holds whatever an emulator reports. It is the reason a lock-free ring
belongs on a real-time hot path.

---

## 3. Context-switch cost: inconclusive

| | FreeRTOS | Ours |
|---|---|---|
| samples | 1,102,479 | 1,036,308 |
| min | 0 | 0 |
| p99 | 176 | 56 |
| p99.9 | 344 | 176 |
| max | 7,999 | 2,852 |
| share in lowest bin | 98.8% | 99.0% |

**Do not quote this comparison.** Both kernels report a minimum of 0. No context
switch costs zero cycles. QEMU resolves almost every switch below its clock
quantum, so 99% of both distributions fall into the lowest bin.

The tail numbers hint that our switch is tighter. But they rest on about 1% of
samples, on an emulator that is not cycle accurate.

This experiment needs hardware, where DWT makes the measurement exact. The
analytic budget in the technical report is about 50 to 60 cycles of switch logic
plus hardware stacking. That is the prediction to check against.

---

## Summary against the objective

| Claim | Verdict | Evidence |
|---|---|---|
| Lower worst-case jitter than FreeRTOS | **Supported** | 29,568 to 3,828 units, 7.7x |
| Lower jitter spread than FreeRTOS | **Supported** | 26,407 to 1 unit |
| Faster hot-path IPC than FreeRTOS | **Supported** | 1.28x ping-pong, 1.29x burst |
| Cannot extend other tasks' latency via IPC | **Supported (structural)** | 0 against 4 critical sections per message, by disassembly |
| Faster raw execution than FreeRTOS | **Not supported** | FreeRTOS uninterrupted block 3,161 against our 3,354 |
| Lower context-switch cost than FreeRTOS | **Not established** | emulation cannot resolve it, needs hardware |
| Cache-line partitioning gains on Cortex-M4 | **Not supported, as expected** | parity, because the part has no data cache |

The objective is determinism, not throughput. The jitter results and the
structural IPC result address it directly.

---

## Reproducing

```bash
cd freertos_baseline && make bench-jitter
```

```bash
qemu-system-arm -machine olimex-stm32-h405 -nographic -icount shift=7 -semihosting-config enable=on,target=native -kernel firmware.elf
```

Do the same for `bench-ipc` and `bench-ctxsw`, and for the matching targets in
`rtos_kernel`.

The full single-kernel run record is in [`TEST_RECORD.md`](TEST_RECORD.md).
