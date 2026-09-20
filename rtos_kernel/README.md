# A Deterministic, Low-Latency RTOS Kernel for ARM Cortex-M4

Target: **STM32F407 (ARM Cortex-M4, 168 MHz)**, STM32F4 Discovery board (hardware);
**QEMU** (`olimex-stm32-h405`) for functional verification only.

---

## 1. What this is

A minimal, preemptive, fixed-priority RTOS kernel built from first principles on
top of the ELL365 bare-metal boot environment, with **worst-case latency as a
first-class, measured design constraint**. The determinism primitives it
implements. Bounded context-switch cost, priority-inheritance synchronisation,
and lock-free single-producer/single-consumer handoff. Are the same techniques
used in production low-latency systems. High-frequency trading motivates the
latency targets and the demonstration workload; no claim is made that the
platform is a trading system.

It reuses, essentially unchanged, the ELL365 layer:
- `startup/startup_stm32f4.s`, 16-entry vector table (already wires `SVC` and
  `PendSV`), `.data`/`.bss` init, `Reset_Handler`.
- `ld/linker.ld`, 1 MB Flash @ `0x08000000`, 128 KB SRAM @ `0x20000000`.

## 2. Feature checklist (mapped to the proposal)

| Objective | Where |
|---|---|
| Preemptive fixed-priority scheduler, round-robin within a band | `rtos.c` |
| O(1) highest-priority selection via `CLZ` on a ready bitmask | `rtos.c` `highest_ready_prio()` |
| Context switch via `PendSV` (saves r4-r11, swaps PSP) at lowest exc. priority | `context.s`, `rtos.c` |
| First task launched via `SVC` | `context.s` `SVC_Handler` |
| 1 kHz `SysTick` system tick | `rtos.c` `rtos_start()` |
| Cycle-accurate context-switch measurement (DWT) + p50/p99/p99.9/max histogram | `context.s`, `stats.c` |
| Worst-case interrupts-disabled window (interrupt-latency basis) | `rtos.c` `rtos_max_irq_disabled_cycles()` |
| Mutex with **priority inheritance** + measured priority-inversion test | `sync.c`, `main.c` |
| **Lock-free, wait-free SPSC** ring buffer with DMB memory barriers | `spsc.c` |
| End-to-end event-to-dispatch latency distribution | `main.c`, `stats.c` |
| Response-time / rate-monotonic **schedulability analysis** | `stats.c` `rtos_schedulability_report()` |
| MPU-based stack-overflow guard (hardware hardening) | `rtos.c` `rtos_mpu_init()`, `rtos_mpu_on_switch()` |
| **Poll-mode scheduling**, suppress the scheduler tick for a bounded region, with tick replay | `poll.c`, `rtos.c` |
| **Cache-line-partitioned lock-free SPSC** with shadow indices | `spsc.c` `spsc_ca_t` |
| Like-for-like **mutex queue baseline** (FreeRTOS-shaped control) | `queue_baseline.c` |
| Benchmarking testbed: jitter, IPC latency, context-switch cost | `bench.c` |
| GPIO driver for external (logic-analyser) jitter verification | `gpio.c` |

## 3. Build and run

```bash
# Toolchain: GNU Arm Embedded (arm-none-eabi-*) + QEMU
make                 # QEMU functional build (8 MHz model) -> firmware.elf/.bin
make qemu            # run under QEMU (Ctrl-A then X to quit)
make debug           # QEMU GDB server on :1234

make hw              # HARDWARE build: 168 MHz tick + MPU stack guard on
make flash           # flash firmware.bin to a connected Discovery board

# Benchmarking testbed - one firmware image per experiment, so that a
# measurement of the scheduler never shares a CPU with unrelated work.
make bench-ctxsw     # Experiment 1: context-switch cost (>=10^6 samples)
make bench-jitter    # Experiment 2: execution jitter, tick-driven vs poll-mode
make bench-ipc       # Experiment 3: 64-byte IPC latency, 3 queue designs
make bench-all       # build and run all three under QEMU, in sequence

# Append HW=1 to build any experiment for the board at 168 MHz:
make bench-jitter HW=1
```

Run under QEMU explicitly (the target enables semihosting output):

```bash
qemu-system-arm -machine olimex-stm32-h405 -nographic \
  -semihosting-config enable=on,target=native -kernel firmware.elf
```

### Flashing the STM32F4 Discovery board

With the ST-Link tools (`brew install stlink` / `apt install stlink-tools`):

```bash
make hw           # build the 168 MHz image with the MPU guard enabled
make flash        # == st-flash write firmware.bin 0x08000000
```

Or with OpenOCD:

```bash
openocd -f board/stm32f4discovery.cfg \
  -c "program firmware.elf verify reset exit"
```

For semihosting output on hardware, run under OpenOCD + `arm-none-eabi-gdb`
with `monitor arm semihosting enable`, or replace `sh_puts` with a UART driver.

The kernel is built at `-O2`. Two freestanding build details matter and are
handled in the `Makefile`:
- `-fno-tree-loop-distribute-patterns` (plus a per-function attribute in
  `libc_stubs.c`): stops GCC from rewriting our own `memset`/`memcpy` byte loops
  into self-calls, which would recurse and overflow the stack.
- We link `libgcc` explicitly (for `__aeabi_uldivmod`, used by the 64-bit math in
  the statistics/analysis code) while staying `-nostdlib` (no libc).

## 4. Architecture

### 4.1 Context switch (`context.s`)
On exception entry the hardware auto-stacks `r0-r3, r12, lr, pc, xpsr`. `PendSV`
stacks the callee-saved `r4-r11`, records the stack pointer in the current TCB,
selects the next task, restores its `r4-r11`, and returns. Tasks run on the PSP,
handlers on the MSP. `PendSV` is programmed at the **lowest** exception priority
so a switch never preempts an active ISR. This is a key determinism property.

The switch is instrumented: it timestamps entry and exit with `rtos_cycles()` and
feeds the delta to `rtos_stats_record_switch()`.

### 4.2 Scheduler (`rtos.c`)
Up to 8 priority bands (higher number = more urgent). Each band is a circular
doubly-linked list of ready tasks; a per-priority `ready_mask` bit is set when a
band is non-empty. The highest ready band is `31 - CLZ(ready_mask)`, a
constant-time selection that compiles to the single-cycle `CLZ` instruction.
Round-robin within a band is a pointer rotation on each tick.

### 4.3 Lock-free SPSC IPC (`spsc.c`)
A wait-free ring buffer for one producer and one consumer, with no locks and no
interrupt masking on the hot path. Free-running monotonic `head`/`tail` counters
index into a power-of-two buffer via a mask. Correctness rests on **memory
ordering**: the producer writes the item, issues a `DMB`, then publishes the
incremented `tail`; the consumer reads `tail`, `DMB`, reads the item, `DMB`, then
publishes the advanced `head`. The `DMB` guarantees the data store is globally
visible before the index store, so the consumer can never observe a new index
with stale data.

### 4.4 Priority inheritance (`sync.c`)
When a task blocks on a mutex held by a lower-priority owner, the owner is
temporarily boosted to the blocker's priority; on release, the owner's base
priority is restored and the mutex is handed directly to the highest-priority
waiter. This bounds the blocking a high-priority task can suffer to the length of
the holder's critical section, independent of unrelated medium-priority tasks.

## 5. Measured results (QEMU functional run)

QEMU's Cortex-M model does not implement the DWT cycle counter, so on QEMU the
kernel automatically falls back to a **SysTick-derived** monotonic cycle counter
(functional, not cycle-accurate). On the STM32F4 Discovery board the DWT counter
is used and the numbers are cycle-accurate. Representative QEMU output:

```
--- Schedulability analysis (RMA) ---
  dispatch: C=50 T=1000 U=0.050
  feed:     C=40 T=1000 U=0.040
  signal:   C=70 T=1000 U=0.070
  Total U=0.160  LL-bound=0.779  => PASS (U bound)
  R(dispatch)=50  <= T? yes
  R(feed)=90      <= T? yes
  R(signal)=160   <= T? yes
  => Task set is SCHEDULABLE (RTA)

--- Priority-inversion test ---
pi_high blocked for 64000 cycles (~8000 us)
Blocking is bounded by pi_low's critical section, NOT by pi_med.

===== Latency report =====
ctx-switch : n=8002 min=0 mean=46 p50=8 p99=176 p99.9=520 max=1176 cyc
            (us)  min=0.00 mean=5.75 p50=1.00 p99=22.00 p99.9=65.00 max=147.00
e2e latency: n=165 ... max=14174 cyc
            (us)  ... max=1771.75
max IRQ-disabled window = 3787 cyc (~473 us)
ctx-switch histogram (cycles):
  [0,8)     5803
  [168,176) 2150
  [512+   )   11
```

Every metric is reported in **cycles first, then microseconds** (converted with
the build-time `RTOS_CPU_MHZ`), and as a **distribution** (min/mean/p50/p99/p99.9/
max) plus a full **histogram**. Because determinism is a property of the tail of
the distribution, not the mean.

## 6. Worst-case interrupt-latency bound

Interrupt latency is bounded by the longest window during which the kernel keeps
interrupts disabled (`PRIMASK=1`). The kernel measures this window continuously
(`rtos_max_irq_disabled_cycles()`); the dominant contributor is the `SysTick`
tick handler, which under interrupt lock:
1. increments the tick counter,
2. walks the delayed-task list decrementing timers (O(number of sleeping tasks)),
3. performs one round-robin rotation and an O(1) `CLZ` reschedule.

The analytic worst case is therefore `A + B·N` cycles, where `N` is the number of
simultaneously-sleeping tasks and `A`, `B` are small constants. A bound that is
linear in the task count and independent of the workload. This measured-typical
vs. analysed-worst-case distinction is exactly the property the proposal targets.

## 7. Schedulability analysis

`rtos_schedulability_report()` applies two standard fixed-priority tests to the
demo workload:
1. **Liu & Layland utilisation bound**: schedulable if `Σ(Cᵢ/Tᵢ) ≤ n(2^{1/n}−1)`
   (sufficient).
2. **Exact response-time analysis**: `Rᵢ = Cᵢ + Σ_{hp(i)} ⌈Rᵢ/Tⱼ⌉·Cⱼ`, iterated to
   a fixed point; schedulable iff `Rᵢ ≤ Tᵢ` for all tasks.

## 8. Files

```
startup/startup_stm32f4.s   ELL365 boot layer (vector table, reset)
ld/linker.ld                ELL365 memory map + kernel symbols
src/context.s               SVC/PendSV context switch + switch timing
src/rtos.{c,h}              scheduler, tasks, tick, cycle counter, MPU guard
src/rtos_internal.h         kernel-private helpers
src/poll.c                  POLL-MODE scheduling: tick suppression + replay
src/sync.c                  semaphores + priority-inheritance mutex
src/spsc.c                  lock-free SPSC ring (plain + cache-partitioned)
src/queue_baseline.c        mutex+semaphore queue - the experimental control
src/stats.c                 histogram/percentiles + schedulability analysis
src/gpio.c, src/gpio.h      STM32F407 GPIO (PD12/PD13) for the jitter probe
src/libc_stubs.c            freestanding memset/memcpy
src/semihosting.h           QEMU semihosting output
src/main.c                  bring-up + build-mode selection (thin)
src/app_demo.c              Feed/Signal/Dispatch pipeline + PI test + reports
src/bench.c                 the three benchmark experiments
docs/BTP_Objective.pdf      project objectives, method and contribution
docs/COMPARISON_FREERTOS.md head-to-head vs FreeRTOS V11.1.0
docs/TEST_RECORD.md         verbatim QEMU test run: commands, output, results
docs/DEFECTS_AND_FIXES.md   defects found and fixed during development
docs/OUTSTANDING_WORK.md    what remains (hardware runs, captures)
docs/technical_report.md    full technical report
```

## 9. Context-switch cycle budget

The proposal targets a full context switch under ~200 cycles (≈1.2 µs at
168 MHz). The switch cost decomposes as:

| Stage | Work | Approx. cycles |
|---|---|---|
| Exception entry (hardware) | auto-stacks r0-r3, r12, lr, pc, xpsr onto PSP | ~12 |
| Save outgoing | `stmdb` of r4-r11 to the task stack + store PSP to TCB | ~11 |
| Schedule | `CLZ` on the ready bitmask -> O(1) band select | ~5 |
| Restore incoming | load PSP from TCB + `ldmia` of r4-r11 | ~11 |
| Exception return (hardware) | auto-unstacks the incoming frame | ~12 |

That is roughly **50-60 cycles** of switch logic plus hardware
stacking/unstacking. Comfortably inside the budget. `context.s` additionally
calls `rtos_cycles()` twice and the stats recorder to *measure* each switch;
those calls are part of the measured figure and are a fixed offset that can be
subtracted, or removed for a production build. The MPU-guard reprogram
(`rtos_mpu_on_switch`) adds a few MPU register writes only on hardware builds.

## 10. The benchmarking testbed

Three experiments, each built as its own firmware image (`bench.c`). A
measurement of the scheduler must not share the CPU with the demo's tasks, or it
stops being a measurement of the scheduler.

### Experiment 1. Context-switch cost (`make bench-ctxsw`)

Two equal-priority tasks `rtos_yield()` in a tight loop, so every yield forces a
real switch. PendSV times each one. Prints the distribution once **≥ 10⁶**
switches are recorded. The tail cannot be stated from a short run.

### Experiment 2. Execution jitter (`make bench-jitter`)

Tests the poll-mode claim. A task toggles PD12 and times blocks of 32 toggles.
Every block runs the identical instruction sequence, so in a perfect world the
distribution is a single spike; anything wider is jitter.

* **Phase A**: Ordinary preemptible execution, with a same-priority partner
  task so the tick's round-robin genuinely deschedules the task under test.
* **Phase B**: The identical loop and the identical task set, wrapped in
  `RTOS_POLL_EXCLUSIVE` poll sections.

Only one variable differs. PD13 is held high across the measured window, so a
logic analyser triggered on PD13 captures exactly the blocks that produced the
printed numbers. The histogram and the waveform are two independent views of
the same claim.

> **Falsifiable prediction:** poll mode removes *interference*; it does not make
> the loop faster. The comparable quantity across the two phases is the
> **minimum**, an uninterrupted block, and the two minima must agree up to the
> cost of the poll machinery itself, which the report prints rather than absorbs.
> The medians are *not* expected to agree: Phase A time-shares with its
> same-priority partner, so most of its blocks legitimately contain a slice of
> that partner, and that time-sharing is the baseline behaviour under test.
> What must collapse is Phase B's **spread**: a single narrow spike against a
> multi-modal Phase A.

### Experiment 3, 64-byte IPC latency (`make bench-ipc`)

Three subjects differing in one variable, the synchronisation strategy:

| | Design | Isolates |
|---|---|---|
| A | `rtos_queue_t`, mutex + copy (FreeRTOS-shaped) |, |
| B | `spsc_t`, lock-free ring | A vs B: the cost of the lock |
| C | `spsc_ca_t`, cache-partitioned + shadow indices | B vs C: the cost of the peer-index load |

The baseline is built **here**, on this kernel, at the same `-O2` and clock, and
is given its most favourable honest form (its fast path skips the semaphores
entirely). A number quoted from another project would confound compiler,
clock and kernel all at once.

Two access patterns are measured, because the shadow-index optimisation is a bet
on the access pattern, not a property of the queue:

* **Ping-pong (depth 1)**: The queue is empty at every receive, so the shadow
  is stale *every* time. The optimisation cannot win here; it can only add a
  branch. This adversarial case is reported, not hidden.
* **Burst (depth 8)**: Fill several, drain several, as a real feed handler
  does. One refresh is amortised over eight operations. This is the case the
  design targets.

Correctness gates the timing: every queue is verified for round-trip byte
integrity, FIFO ordering and full/empty edge behaviour before any figure is
printed.

### Results from the testbed

Emulation results; absolute cycle figures are reproduced on hardware with
`make bench-* HW=1`. Full run record in [`docs/TEST_RECORD.md`](docs/TEST_RECORD.md).

**Experiment 2. Jitter.** 4000 blocks per phase, identical task set, only the
poll section differing:

| Quantity | Phase A (tick-driven) | Phase B (poll mode) |
|---|---|---|
| peak-to-peak spread | 27,806 | **1** |
| worst observed block | 31,160 | **3,828** |
| uninterrupted block (min) | 3,354 | 3,827 |
| distribution shape | multi-modal (4 modes) | **single bin, 4000/4000** |

Worst-case improvement **8.1×**. The comparable quantity across phases is the
minimum; the 473-unit gap between the two minima is the measured
`poll_enter`+`poll_exit` cost.

**Experiment 3, 64-byte IPC.** Correctness gate: **PASS** for all three queues.

| Comparison | Ping-pong (depth 1) | Burst (depth 8) |
|---|---|---|
| lock-free vs mutex queue | **1.26×** | **1.26×** |
| cache-partitioned vs plain | 0.99× | 0.99× |

Removing the lock is worth ~26%. The cache-line partitioning measures at parity:
the Cortex-M4 has no data cache and single-cycle SRAM, so the load the shadow
indices remove costs about as much as the branch and store replacing it. It is a
portability and multi-core property, not claimed as an M4 speed-up.

### Against FreeRTOS

FreeRTOS V11.1.0 was built and measured on the same boot layer, clock,
instrumentation and workload, see [`freertos_baseline/`](../freertos_baseline/)
and [`docs/COMPARISON_FREERTOS.md`](docs/COMPARISON_FREERTOS.md).

| | FreeRTOS | Ours, tick-driven | Ours, poll mode |
|---|---|---|---|
| worst observed block | 29,568 | 31,160 | **3,828** |
| peak-to-peak jitter | 26,407 | 27,806 | **1** |
| uninterrupted block (min) | **3,161** | 3,354 | 3,827 |

| 64-byte IPC, per message | Ping-pong | Burst |
|---|---|---|
| FreeRTOS `xQueueSend`/`xQueueReceive` | 9,290 | 9,445 |
| Our lock-free SPSC | **7,285** | **7,317** |

Worst-case jitter **7.7×** better; hot-path IPC **1.28×** faster. FreeRTOS is
~6% faster on the uninterrupted path. This kernel is not the faster kernel at
raw execution, and is not claimed to be. The context-switch comparison is
inconclusive under emulation and needs hardware.

### The determinism argument is structural, not statistical

The strongest reason to put a lock-free ring on a real-time hot path is not
throughput. It is that it **never disables interrupts**, so it cannot extend
any other task's worst-case latency. A mutex must. That is a property of the
generated code, and it is checkable directly:

```bash
arm-none-eabi-objdump -d build/spsc.o | grep -c cpsid            # -> 0
arm-none-eabi-objdump -d firmware.elf | \
    sed -n '/<rtos_queue_try_send>:/,/^$/p' | grep -c 'bl.*mutex' # -> non-zero
```

### Running the benchmarks under QEMU

QEMU implements no DWT **and is not cycle-accurate**. Its virtual clock advances
in blocks, not per retired instruction. That quantises short timed regions onto
identical values. For a QEMU run to say anything at all about
relative cost, pass `-icount`, which ties virtual time to *instruction count*:

```bash
qemu-system-arm -machine olimex-stm32-h405 -nographic -icount shift=7 \
  -semihosting-config enable=on,target=native -kernel firmware.elf
```

That is still a **proxy, not a cycle count**: it models no memory stall, no DMB
pipeline drain, no flash wait state and no bus contention. Use it to compare the
*shape* of the designs and to confirm correctness. **The numbers that go in the
report come from the board** (`make bench-* HW=1`), where the DWT cycle counter
is real.

## 11. QEMU vs. hardware

QEMU verifies **functional correctness**: scheduling, preemption, priority
inheritance, SPSC ordering, and the pipeline. Cycle-accurate **timing** (context
switch < ~200 cycles target, tail-latency distribution over ≥10⁶ samples, the
interrupt-latency bound, and MPU stack-overflow faulting) is characterised on the
STM32F4 Discovery board, where the DWT cycle counter is real and the core runs at
168 MHz. `make hw` sets `-DRTOS_CPU_MHZ=168` (correct 1 kHz tick and microsecond
conversions) and `-DRTOS_ENABLE_MPU=1` (per-task stack-overflow guard).

## 12. Authors and license

Authored by **Aakarsh D Reja (2023EE11151)** and **Jayesh Narayanan
(2023EE11048)** for ELD411 (B.Tech Project), Department of Electrical
Engineering, IIT Delhi, under Prof. Kaushik Saha.

Released under the MIT License, see [`LICENSE`](LICENSE).

A full technical write-up is in [`docs/technical_report.md`](docs/technical_report.md).
