# Hardware Results: Micro-Trade OS vs FreeRTOS

Measured on real silicon, head to head, on the same board, at the same clock,
with the same compiler, flags, boot code, measurement code and workload. The
kernel is the only variable.

**Bottom line.** On the same chip, Micro-Trade OS cuts the worst-case execution
time of a critical loop from **100,431 cycles** under FreeRTOS to **232 cycles**
(**433× lower**), reduces its jitter from **100,242 cycles to 1 cycle**, and
passes a 64-byte message **1.72× faster** than FreeRTOS's queue without ever
disabling interrupts. FreeRTOS remains marginally faster on raw, uninterrupted
execution (by about 2%); the claim of this project is **determinism**, not raw
speed.

---

## 1. Test setup

| | |
|---|---|
| Board | WeAct Black Pill V3, **STM32F411CEU6** |
| Core | ARM Cortex-M4F with MPU and DWT cycle counter |
| Clock | **100 MHz**, PLL from the 25 MHz crystal (confirmed in every boot log) |
| Flash / RAM | 512 KB / 128 KB, 3 Flash wait states, ART accelerator on |
| Toolchain | Arm GNU Toolchain 15.2, `-O2`, soft-float, no C library |
| Debug / output | ST-Link V2 over SWD, OpenOCD 0.12, semihosting (printing happens outside timed sections) |
| Our kernel | Micro-Trade OS, this repository, MPU stack guard **on** unless stated |
| Baseline | **FreeRTOS V11.1.0**, `ARM_CM3` port, 1 kHz tick, preemption + time slicing, CLZ task selection, `heap_4`, priority-inheritance mutexes |
| Clock source for timings | DWT `CYCCNT` (our kernel); SysTick-derived cycle count (FreeRTOS baseline). Both count CPU cycles at 100 MHz. |

At 100 MHz, **1 cycle = 10 ns** and **100 cycles = 1 µs**. Cycle counts belong to
the Cortex-M4 core, so they carry over directly to other STM32F4 parts such as
the STM32F407 Discovery; only the microsecond figures depend on the clock.

Raw output of every run is in the repository root: `results_blackpill_*.txt`.

---

## 2. Results at a glance

| Measure | FreeRTOS V11.1.0 | Micro-Trade OS | Better |
|---|---|---|---|
| **Worst-case block** in a critical loop (Exp. 2) | 100,431 cyc (1,004 µs) | **232 cyc (2.32 µs)**, poll mode | **Ours, 433×** |
| **Jitter** (max − min) of that loop | 100,242 cyc | **1 cyc** | **Ours** |
| Blocks interrupted by the scheduler (of 4,000) | 7 | **0** | **Ours** |
| **64-byte message**, ping-pong (Exp. 3) | 667 cyc (6.67 µs) | **387 cyc (3.87 µs)**, lock-free | **Ours, 1.72×** |
| 64-byte message, burst of 8 | 669 cyc | **389 cyc** | **Ours, 1.72×** |
| Interrupt-disabling sections per message | 4 | **0** | **Ours** |
| Context switch (Exp. 1) | 73–82 cyc *(selection step only)* | **57–65 cyc** *(full switch)*, 84–95 with MPU guard | **Ours** (see §3) |
| Uninterrupted loop block (raw speed) | **189 cyc** | 193 cyc | FreeRTOS, by 2% |
| Hardware stack-overflow protection | No (its optional software check is off in this baseline, and only detects an overflow after it has happened) | **Yes**, MPU guard on every switch | **Ours** |
| Suppress the scheduler tick for a critical region | **Not available** | **Yes**, poll mode with tick replay | **Ours** |

---

## 3. Experiment 1: context-switch cost

Two equal-priority tasks yield to each other in a tight loop, so every yield is
a real switch. Over one million switches are recorded per run.

| Kernel | Samples | Min | Mean | Max | Spread |
|---|---|---|---|---|---|
| FreeRTOS V11.1.0 | 1,084,696 | 73 | 74 | 82 | 9 |
| **Ours, MPU guard off** | 1,217,493 | **57** | **57** | **65** | 8 |
| Ours, MPU guard on | 1,080,410 | 84 | 84 | 95 | 11 |

**How to read this.** The two kernels time different windows, and the
difference favours FreeRTOS:

- **FreeRTOS** is timed by its own trace hooks (`traceTASK_SWITCHED_OUT` →
  `traceTASK_SWITCHED_IN`), which run inside `vTaskSwitchContext()`. That
  window covers **only the choice of the next task**. Saving and restoring the
  registers happen outside it and are not counted.
- **Ours** is timed across the **whole `PendSV` switch**: saving r4–r11,
  choosing the next task, restoring its registers and, when enabled,
  reprogramming the MPU guard.

So our complete switch (57–65 cycles) costs less than FreeRTOS's selection step
alone (73–82 cycles). The MPU stack guard, which FreeRTOS does not have, costs
**about 27 cycles per switch**. Even with it on, the full switch (84–95 cycles)
is well inside the project's 200-cycle target.

---

## 4. Experiment 2: execution jitter (the central result)

A task runs a block of 32 GPIO toggles, an identical instruction sequence every
time, and times each block with the cycle counter. 4,000 blocks per run. A
same-priority "noise" task competes for the CPU, as in any real multi-task
system. Any spread in block times is interference, not variation in the work.

| | FreeRTOS | Ours, tick-driven (Phase A) | **Ours, poll mode (Phase B)** |
|---|---|---|---|
| Fastest block | 189 cyc | 193 cyc | 231 cyc |
| **Slowest block** | 100,431 cyc (1.004 ms) | 100,577 cyc (1.006 ms) | **232 cyc (2.32 µs)** |
| **Jitter (max − min)** | 100,242 cyc | 100,384 cyc | **1 cyc** |
| Blocks hit by the scheduler | 7 of 4,000 | 8 of 4,000 | **0 of 4,000** |
| Ticks lost | — | — | **0** (22 suppressed ticks replayed) |

**What happens.** In FreeRTOS, and in our kernel's ordinary tick-driven mode,
the 1 kHz tick periodically hands the CPU to the noise task for a full time
slice. Most blocks finish in about 190 cycles, but a few are stretched to about
1 ms. The worst case is **520 times the typical case**, and a real-time budget
has to be written against the worst case.

In poll mode, the task opens a poll section around each block. The kernel stops
the tick from firing, refuses to switch tasks, and masks other interrupts for
the length of the block, then replays the suppressed ticks afterwards. Every
one of the 4,000 blocks took 231 or 232 cycles. No time was lost: 22 ticks were
suppressed and all 22 were replayed, with zero overruns and zero violations.

**Against FreeRTOS:** the worst case drops from **100,431 to 232 cycles
(433× lower)**, and jitter from **100,242 cycles to 1**. FreeRTOS has no way to
suppress its scheduler tick for a critical region, so this is a capability
FreeRTOS does not offer, not a tuning difference.

**The cost, reported rather than hidden.** Poll mode does not make the loop
faster. Its fastest block (231 cycles) is 38 cycles slower than the
tick-driven one (193), which is the cost of entering and leaving a poll
section: about 1 cycle per toggle. Our tick-driven mode measures within 0.2% of
FreeRTOS in the worst case, which confirms the in-house baseline is an honest
stand-in for a conventional RTOS.

---

## 5. Experiment 3: 64-byte message passing

Each design passes a 64-byte message (the size of a market-data record) between
tasks. Every queue first passes a correctness check: byte-for-byte integrity,
first-in-first-out order, and correct full/empty behaviour. All passed on
hardware, FreeRTOS included. Each figure is the exact median of 200 batches of
256 round trips.

| Queue | Ping-pong (depth 1) | Burst (depth 8) | Disables interrupts? |
|---|---|---|---|
| FreeRTOS `xQueueSend` / `xQueueReceive` | 667 cyc (6.67 µs) | 669 cyc | Yes, 4 times per message |
| Ours: mutex queue (FreeRTOS-shaped control) | 493 cyc | 495 cyc | Yes, 4 times per message |
| **Ours: lock-free SPSC ring** | **387 cyc (3.87 µs)** | **389 cyc** | **Never** |
| Ours: lock-free, cache-partitioned | 391 cyc | 390 cyc | Never |

| Comparison | Ping-pong | Burst |
|---|---|---|
| **Lock-free vs FreeRTOS queue** | **1.72×** | **1.72×** |
| Lock-free vs our mutex queue (the cost of the lock alone) | 1.27× | 1.27× |
| Our mutex queue vs FreeRTOS queue | 1.35× | 1.35× |
| Cache-partitioned vs plain lock-free | 0.98× | 0.99× |

**Why it matters for real-time.** Speed is the smaller half of the result. A
queue that disables interrupts can delay *every other task in the system*,
including ones that never touch the queue. FreeRTOS's queue does this four
times per message; the lock-free ring never does, which can be checked in the
disassembly (`arm-none-eabi-objdump -d build/spsc.o | grep -c cpsid` → `0`).

**Stability.** For every queue, the slowest batch equalled the median (or
differed by 1 cycle) across all 200 batches.

**Cache partitioning** measures at parity, as predicted. The Cortex-M4 has no
data cache, so padding cannot produce a cache effect here. It is kept for
portability: on a cached Cortex-M7 or a multi-core part, it prevents false
sharing. It is not claimed as a speed-up on this chip.

---

## 6. Other properties measured on hardware

| Property | Result |
|---|---|
| Longest time with interrupts disabled | **168 cycles (1.68 µs)** in the context-switch benchmark; **402 cycles (4.02 µs)** under the full demo workload. This is the measured basis for the kernel's interrupt-latency bound. |
| Priority inheritance | A high-priority task blocked on a mutex held by a low-priority one waited **98 cycles**. It was not delayed by an unrelated medium-priority task, which is the priority-inversion failure the mechanism prevents. |
| Schedulability analysis | The demo task set (utilisation 0.160 against a bound of 0.779) is shown schedulable by both the utilisation test and response-time analysis, printed at boot before the run starts. |
| Context switch inside a live workload | 90–98 cycles across 32,000+ switches while the trading-pipeline demo was running |
| Clock bring-up | 100 MHz from the external crystal on every boot, verified in the boot log and in the clock registers |

---

## 7. Why Micro-Trade OS is better for real-time work

1. **It bounds the worst case, not just the average.** Poll mode turns a
   1 ms worst case into 2.32 µs. For a braking controller, a flight
   controller or a trading engine, the worst case is the only number that
   matters, and FreeRTOS cannot offer this guarantee.
2. **It does not lose time to get there.** Suppressed ticks are counted and
   replayed, so delays and timeouts stay correct. The rest of the system sees a
   scheduler that paused, not one that skipped.
3. **Its fast path never blocks other tasks.** The lock-free ring passes
   messages 1.72× faster than FreeRTOS's queue and never disables interrupts,
   so one task's messaging cannot add latency to another task.
4. **It is protected in hardware.** The MPU guard stops a stack overflow at the
   moment it happens, instead of letting it silently corrupt a neighbouring
   task. The FreeRTOS baseline has no MPU guard; FreeRTOS's optional software
   stack check only notices an overflow after the damage is done.
5. **It measures itself.** Every build reports its own context-switch
   distribution, its longest interrupts-off window and its schedulability
   analysis, from the cycle counter. The guarantees are measured, not assumed.
6. **It is small and self-contained.** About 12 KB of Flash and 9 KB of RAM,
   no vendor HAL, no CMSIS, no C library: every instruction from reset upward
   is in this repository.

---

## 8. Where FreeRTOS is as good or better

Stated so the comparison is fair:

- **Raw, uninterrupted speed.** FreeRTOS's fastest loop block is 189 cycles
  against our 193 (about 2%). Our kernel is not the faster kernel at raw
  execution and is not claimed to be.
- **Breadth and maturity.** FreeRTOS supports dozens of architectures, has
  timers, event groups, stream buffers, a TCP/IP stack and years of field use.
  This kernel targets one class of chip and one problem: determinism.
- **Poll mode has a price.** Determinism inside a poll section costs about
  1 cycle per toggle in this benchmark, and the section is cooperative: the task
  must leave it. The kernel enforces a budget and reports any overrun or
  violation, so the obligation is visible.

---

## 9. Features of Micro-Trade OS

**Scheduling**
- Preemptive, fixed-priority scheduler with 8 priority levels and round-robin
  within a level
- O(1) task selection: one `CLZ` instruction on a ready bitmask
- Context switch through `PendSV` at the lowest exception priority, first task
  launched through `SVC`
- 1 kHz system tick, sleeping-task delays
- **Poll mode**: suppress the tick for a bounded critical region, block task
  switches, optionally mask interrupts, then replay the suppressed ticks; with
  a per-section cycle budget and overrun/violation accounting

**Synchronisation and messaging**
- Mutex with **priority inheritance**, and counting semaphores
- **Lock-free, wait-free single-producer/single-consumer ring buffer**, with
  `DMB` memory barriers and no interrupt masking
- **Cache-line-partitioned** variant with shadow indices, for cached or
  multi-core parts
- A FreeRTOS-shaped mutex queue, kept as the experimental control

**Safety and timing analysis**
- **MPU stack-overflow guard**, reprogrammed for the incoming task on every
  switch
- Rate-monotonic and response-time **schedulability analysis**, printed at boot
- Longest **interrupts-disabled window** tracked continuously

**Measurement**
- Cycle-accurate timing from the DWT counter on hardware, with an automatic
  SysTick-based fallback under QEMU
- Latency distributions as min / mean / p50 / p99 / p99.9 / max with histograms
- A benchmarking testbed of three experiments, each built as its own firmware
  image, plus FreeRTOS V11.1.0 running the same experiments

**Platform**
- Runs on the STM32F4 Discovery (STM32F407, 168 MHz) and the WeAct Black Pill
  (STM32F411, 100 MHz), selected with `BOARD=disc` or `BOARD=blackpill`;
  functional testing under QEMU
- Clock bring-up from the external crystal, with automatic fallback to the
  internal oscillator
- No vendor HAL, no CMSIS, no C standard library

---

## 10. Defects found by hardware testing

Two bugs were invisible under QEMU and found on the board:

1. **MPU guard faulted on the first switch.** The guard rounded each stack's
   start address *down* to a 32-byte boundary, so its no-access zone could
   cover the end of the variable just before the stack. Here that was the
   kernel's own MPU-enabled flag, read on every switch. The guard now rounds
   *up*, so it always lies inside the task's own stack.
2. **IPC speed-ups printed as 1.00×.** The medians were read from a 256-cycle
   histogram bin, in which all three queues fell. Medians are now computed
   exactly from the stored samples.

---

## 11. Reproducing these results

With the board wired as in
[`HARDWARE_TESTING_BLACKPILL.md`](HARDWARE_TESTING_BLACKPILL.md):

```bash
cd rtos_kernel
make bench-ctxsw  HW=1 BOARD=blackpill && make run-hw
make bench-jitter HW=1 BOARD=blackpill && make run-hw
make bench-ipc    HW=1 BOARD=blackpill && make run-hw

cd ../freertos_baseline
make bench-ctxsw  HW=1 BOARD=blackpill && make run-hw
make bench-jitter HW=1 BOARD=blackpill && make run-hw
make bench-ipc    HW=1 BOARD=blackpill && make run-hw
```

Context switch with the MPU guard off:

```bash
cd rtos_kernel
make clean && make HW=1 BOARD=blackpill CPU_MHZ=100 EXTRA_DEFS="-DRTOS_BENCH_CTXSW=1" && make run-hw
```
