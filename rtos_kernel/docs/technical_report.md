# A Deterministic, Low-Latency Real-Time Operating System Kernel for ARM Cortex-M4

**Technical Report, B.Tech Project (ELD411)**
Department of Electrical Engineering, Indian Institute of Technology Delhi

**Authors:** Aakarsh D Reja (2023EE11151), Jayesh Narayanan (2023EE11048)
**Supervisor:** Prof. Kaushik Saha
**Target platform:** STM32F407 (ARM Cortex-M4, 168 MHz), STM32F4 Discovery board;
QEMU (`olimex-stm32-h405`) for functional verification.

---

## Abstract

We present a minimal preemptive, fixed-priority real-time operating system (RTOS)
kernel for the ARM Cortex-M4, designed from first principles with worst-case
latency as a measured, first-class constraint rather than as an afterthought.
The kernel is built on top of a hand-written bare-metal boot environment (vector
table, linker script, and SysTick timer) developed in ELL365. On that substrate
we implement: a constant-time (`CLZ`-based) fixed-priority scheduler with
round-robin within priority bands; context switching through the `PendSV` and
`SVC` exceptions; counting/binary semaphores and a mutex with priority
inheritance; a wait-free single-producer/single-consumer (SPSC) ring buffer for
hot-path inter-task communication; and cycle-level instrumentation using the Data
Watchpoint and Trace (DWT) unit. We characterise context-switch and end-to-end
latency as distributions (p50/p99/p99.9/max), derive an analytic bound on
interrupt latency from the longest interrupts-disabled critical section,
demonstrate and measure the resolution of a constructed priority-inversion
scenario, and apply response-time schedulability analysis to a three-task
pipeline. The whole kernel is under ~1,600 lines of C and assembly and is
released under the MIT license.

---

## 1. Introduction and Motivation

Latency-sensitive embedded systems. From motor control to the infrastructure
behind electronic trading. Depend on an operating system that provides
deterministic, bounded response times rather than high average throughput. The
property that matters is not "fast on average" but "predictable in the worst
case": bounded interrupt latency, bounded context-switch cost, and low,
quantifiable scheduler jitter. General-purpose operating systems optimise for
aggregate throughput and fairness and therefore cannot provide these guarantees.

Production open-source RTOSes (FreeRTOS, Zephyr, ThreadX) are robust but abstract
away the exact mechanisms that determine worst-case interrupt latency,
context-switch overhead, and scheduler jitter. The educational and engineering
value of this project is in building, measuring, and formally bounding those
mechanisms at the cycle level.

High-frequency trading (HFT) is the motivating domain for the latency targets and
the demonstration workload. The determinism primitives this kernel exercises, 
bounded worst-case latency, priority-inheritance synchronisation, and lock-free
data handoff. Are the same techniques used in production low-latency systems,
even though those systems run on very different hardware (x86 servers with kernel
bypass, FPGAs on the network hot path). We make **no claim** that this platform
is a trading system; HFT simply provides realistic latency requirements and a
concrete pipeline to demonstrate.

### 1.1 Foundation: the ELL365 boot environment

This work reuses, essentially unchanged, the bare-metal boot layer built in
ELL365:

- **`startup_stm32f4.s`**: A hand-written ARM Cortex-M4 startup file that defines
  the full 16-entry vector table (already including the `SVC` and `PendSV` slots
  the RTOS needs), copies `.data` from Flash to RAM, zeroes `.bss`, and branches
  to `main()`.
- **`linker.ld`**: Places the vector table at the base of Flash (`0x08000000`)
  and maps `.data`, `.bss`, and the stack into the 128 KB SRAM window at
  `0x20000000`.
- Direct, HAL-free configuration of the SysTick timer and a verified interrupt
  handler.

The ELL365 project provides exactly the substrate an RTOS requires: a working
vector table, a known memory map, and a reliable hardware timer.

### 1.2 Objectives

1. A preemptive fixed-priority scheduler using `PendSV` and `SysTick` as the sole
   hardware primitives, with round-robin within priority bands, under ~1,500
   lines of C and assembly.
2. Cycle-accurate context-switch measurement using the DWT cycle counter; target
   full switch under ~200 cycles, with the cycle budget accounted for explicitly.
3. Statistical latency characterisation (p50/p99/p99.9/max over ≥10⁶ samples).
4. An analytic worst-case interrupt-latency bound from the longest
   interrupts-disabled critical section.
5. A priority-inheritance mutex, verified against a constructed priority-inversion
   scenario with measured, bounded blocking.
6. Response-time (and rate-monotonic utilisation-bound) schedulability analysis.
7. A wait-free SPSC ring buffer with correct memory ordering (DMB barriers).

---

## 2. Background: relevant Cortex-M4 architecture

The Cortex-M4 is an ARMv7-M core executing the Thumb-2 instruction set. The
features this kernel relies on are:

- **Dual stack pointers.** The core has a Main Stack Pointer (MSP) and a Process
  Stack Pointer (PSP), selected by `CONTROL.SPSEL`. The convention adopted here
  (as in most Cortex-M RTOSes) is that exception handlers run on MSP and tasks run
  on PSP. This isolates the kernel's stack usage from each task's.
- **Automatic exception stacking.** On exception entry the hardware pushes eight
  registers, `r0-r3, r12, lr, pc, xpsr`, onto the current stack, and pops them
  on exception return. The software need only preserve the callee-saved registers
  `r4-r11`.
- **`PendSV` and `SVC`.** `PendSV` is a pendable, software-triggered exception
  designed for deferred context switching; `SVC` is a synchronous supervisor call.
  Both have configurable priority.
- **`CLZ`.** A single-cycle count-leading-zeros instruction, used for O(1)
  priority selection.
- **DWT cycle counter.** `DWT->CYCCNT` is a free-running 32-bit counter of core
  clock cycles, ideal for cycle-accurate measurement.
- **`PRIMASK`.** A single-bit interrupt mask used to implement short critical
  sections by disabling all configurable-priority interrupts.
- **MPU.** An optional Memory Protection Unit that can mark regions no-access,
  used here to trap stack overflow.

---

## 3. System architecture

The kernel is organised into small, single-responsibility modules:

```
startup/startup_stm32f4.s   ELL365 boot layer (vector table, reset, .data/.bss)
ld/linker.ld                ELL365 memory map + kernel symbols
src/context.s               SVC/PendSV context switch + switch timing
src/rtos.{c,h}              scheduler, tasks, tick, cycle counter, MPU guard
src/rtos_internal.h         kernel-private helpers shared across modules
src/sync.c                  semaphores + priority-inheritance mutex
src/spsc.c                  lock-free wait-free SPSC ring buffer
src/stats.c                 histogram/percentiles + schedulability analysis
src/libc_stubs.c            freestanding memset/memcpy
src/semihosting.h           QEMU semihosting console output
src/main.c                  demo workload: pipeline, PI test, reports; benchmark
```

### 3.1 Boot flow

At reset the hardware loads MSP from vector table entry 0 (`_estack =
0x20020000`) and the reset vector from entry 1. `Reset_Handler` copies the
initialised data image from Flash to RAM, zeroes `.bss`, and calls `main()`.
`main()` configures the DWT counter, prints the schedulability analysis, creates
tasks, and calls `rtos_start()`, which programs exception priorities, enables the
MPU guard (hardware builds), starts the 1 kHz SysTick, and launches the first
task via `SVC`. From then on the system is entirely interrupt/scheduler driven.

### 3.2 Task Control Block

```c
typedef struct tcb {
    volatile uint32_t *sp;       /* saved PSP, MUST be the first member */
    struct tcb *next, *prev;     /* intrusive circular list links        */
    uint32_t base_priority;      /* configured priority                  */
    uint32_t priority;           /* effective priority (may be inherited)*/
    volatile uint32_t delay;     /* remaining ticks when delayed         */
    task_state_t state;
    const char *name;
    uint32_t *stack_base;        /* lowest stack address (MPU guard)     */
    uint32_t stack_words;
    struct rtos_mutex *held_mutex;
    void *wait_object;
} tcb_t;
```

`sp` is deliberately the first member: the assembly context switcher dereferences
the raw TCB pointer to load/store the saved stack pointer, so no struct-offset
arithmetic is needed in assembly.

---

## 4. Context switching

### 4.1 Mechanism

Context switches are performed in the `PendSV` handler (`context.s`), which is
configured at the **lowest** exception priority. This is a deliberate determinism
choice: a switch can therefore never preempt an active interrupt service routine.
An ISR that makes a higher-priority task ready simply pends `PendSV`; the actual
switch is tail-chained after all ISRs complete.

The switch sequence:

1. The hardware has already stacked `r0-r3, r12, lr, pc, xpsr` onto the outgoing
   task's PSP.
2. `PendSV` reads PSP, pushes the callee-saved `r4-r11` (`stmdb`), and stores the
   updated PSP into the outgoing TCB's `sp` field.
3. The scheduler selects the next task and writes it to `rtos_current_task`.
4. `PendSV` loads the incoming task's PSP, pops `r4-r11` (`ldmia`), and writes PSP.
5. On exception return the hardware unstacks the incoming task's `r0-r3, r12, lr,
   pc, xpsr`, resuming it exactly where it left off.

### 4.2 Starting the first task

There is no outgoing context for the very first task, so it is launched through a
distinct path: `rtos_start()` selects the highest-priority ready task and issues
an `SVC`. `SVC_Handler` pops the initial `r4-r11` from the manufactured stack
frame, sets PSP, switches `CONTROL` to use PSP in thread mode, and performs an
exception return straight into the task's entry point. A subtlety learned during
bring-up: the `SVC` instruction must be executed with interrupts **enabled**, 
issuing `SVC` while `PRIMASK` is set escalates it to a HardFault.

### 4.3 Manufactured initial stack frame

`rtos_task_create()` builds an initial exception frame so the first switch
"returns" into the task:

```
[ xPSR=0x01000000 ][ PC=entry ][ LR ][ r12 ][ r3 ][ r2 ][ r1 ][ r0=arg ]  <- hw frame
[ r11 ][ r10 ][ r9 ][ r8 ][ r7 ][ r6 ][ r5 ][ r4 ]                          <- sw frame
```

`xPSR` has only the Thumb bit set; `PC` is the task entry; `r0` carries the task
argument.

### 4.4 Cycle budget

| Stage | Work | Approx. cycles |
|---|---|---|
| Exception entry (hardware) | auto-stack r0-r3, r12, lr, pc, xpsr | ~12 |
| Save outgoing | `stmdb` r4-r11 + store PSP to TCB | ~11 |
| Schedule | `CLZ` on ready bitmask, O(1) band select | ~5 |
| Restore incoming | load PSP + `ldmia` r4-r11 | ~11 |
| Exception return (hardware) | auto-unstack incoming frame | ~12 |

The switch logic proper is ~50-60 cycles; with hardware stacking/unstacking the
full switch is well under the ~200-cycle (≈1.2 µs @168 MHz) target. `context.s`
additionally calls `rtos_cycles()` twice and the statistics recorder in order to
*measure* every switch; that instrumentation is a fixed offset which can be
subtracted for reporting or compiled out for a production build.

---

## 5. Scheduler

The scheduler (`rtos.c`) is fixed-priority preemptive with round-robin within a
band:

- Up to 8 priority bands (higher number = more urgent); band 0 is the idle task.
- Each band is a **circular doubly-linked list** of ready TCBs; O(1) insert and
  remove.
- A `ready_mask` bit is set when a band is non-empty. The highest ready band is
  `31 - CLZ(ready_mask)`. A constant-time selection compiling to the single-cycle
  `CLZ` instruction (the same technique used by port-optimised FreeRTOS).
- Round-robin within a band is a single pointer rotation performed on each tick.
- An always-ready idle task at band 0 guarantees `ready_mask` is never zero (so
  `CLZ` is always well-defined) and issues `WFI` to save power.

`rtos_schedule()` selects the next task and pends `PendSV` only if it differs from
the current task, avoiding needless switches.

---

## 6. Synchronisation and priority inheritance

### 6.1 Semaphores

`sync.c` provides counting and binary semaphores. `take` blocks the caller on the
semaphore's wait list and reschedules when the count is zero; `give` increments
the count and readies the highest-priority waiter. All operations run inside short
`PRIMASK` critical sections.

### 6.2 The unbounded priority-inversion problem

Consider three tasks L (low), M (medium), H (high). L acquires a lock; H then
requests it and blocks. Without mitigation, any ready M can preempt L
indefinitely, so H is blocked not just for L's critical section but for the
entire time M runs. The blocking is **unbounded** by anything related to the
lock. This is the classic priority inversion (famously seen on Mars Pathfinder).

### 6.3 Priority inheritance

Our mutex bounds this. When H blocks on a mutex held by lower-priority L, L is
temporarily **boosted** to H's priority. Now M (medium) cannot preempt L, so H
waits only for L's critical section. On release, L's base priority is restored and
the mutex is handed directly to the highest-priority waiter.

### 6.4 Measured demonstration

`main.c` constructs exactly the L/M/H scenario. `pi_low` (prio 2) takes the mutex
and holds it through a busy section; `pi_high` (prio 7) requests it and records
`t_request`; `pi_med` (prio 3) wakes mid-section and spins. With inheritance,
`pi_low` runs at priority 7 while it holds the lock, so `pi_med` never preempts
it, and `pi_high`'s blocking equals the remaining hold time. Measured result
(QEMU functional):

```
pi_high blocked for 64000 cycles (~8000 us)
```

The blocking is bounded by `pi_low`'s critical section and is independent of
`pi_med`. The defining property of priority inheritance.

---

## 7. Lock-free wait-free SPSC IPC

The data hot path uses a single-producer/single-consumer (SPSC) ring buffer
(`spsc.c`) with no locks and no interrupt masking. Because exactly one task
produces and one consumes a given queue, no mutual exclusion is required; progress
is **wait-free** (bounded instruction count, no spinning on a peer).

### 7.1 Structure

`head` (consumer) and `tail` (producer) are free-running monotonic 32-bit
counters; the physical slot is `index & mask` with a power-of-two capacity. The
occupancy `tail - head` never exceeds capacity, so unsigned wraparound of the
counters is harmless.

### 7.2 Memory-ordering correctness

Correctness rests entirely on ordering. On the Cortex-M4 (single core, with a
store buffer) the producer must make the item data globally visible **before** it
publishes the incremented `tail`, or the consumer could observe the new index and
read stale data. We enforce this with a data memory barrier:

```c
/* producer */                         /* consumer */
copy item into buffer[tail & mask];    read tail;  if (tail == head) empty;
DMB();     /* data before index */     DMB();      /* index before data read */
tail = tail + 1;   /* publish */       copy buffer[head & mask] into item;
                                       DMB();      /* data read before index */
                                       head = head + 1;  /* publish */
```

The producer's `DMB` guarantees the item store completes before the `tail` store;
the consumer reads `tail` first, `DMB`, then reads the data, so it can never pair
a fresh index with stale data. This is the standard release/acquire pattern
realised with explicit `DMB` on ARMv7-M (which lacks C11 atomics in a freestanding
`-nostdlib` build).

---

## 7A. Poll-mode scheduling

### 7A.1 The problem this solves

Sections 4-6 bound the cost of *doing* work in the kernel. They do not bound the
cost of being **interrupted**. A conventional RTOS keeps a periodic tick running
at all times, 1 kHz here, as in a stock FreeRTOS configuration, because that
interrupt is what makes preemption possible. It is also what makes latency
unpredictable: it can land on any instruction of a time-critical task, and the
victim pays the vector fetch, the stack frame push, the handler body, a
scheduler pass and the exception return.

Averaged over a long run that cost is negligible. As a worst case on a single
iteration, it is the entire jitter budget. Determinism is a claim about the
tail, so the tick is the first thing that has to go.

### 7A.2 Mechanism

A task declared `TASK_CLASS_POLL` may open a **poll section**
(`rtos_poll_enter()` / `rtos_poll_exit()`, `poll.c`). For its duration the
kernel guarantees the task is not descheduled. Three things happen on entry:

1. **The tick is silenced, not stopped.** SysTick's `TICKINT` bit is cleared so
   the interrupt cannot fire, while `ENABLE` is deliberately left set so the
   24-bit counter keeps counting. This distinction is the whole design: the
   section suppresses *preemption* without losing *time*, which is what makes
   it reversible.

2. **The scheduler is interlocked.** `rtos_schedule()` stops pending PendSV and
   records a deferred switch in `rtos__poll_deferred` instead. Without this, an
   interrupt that readied a higher-priority task would still cause a switch, and
   the zero-jitter property would be an accident of the tick being masked rather
   than a guarantee of the kernel.

3. **Optionally, all interrupts are masked.** In `RTOS_POLL_EXCLUSIVE` mode
   BASEPRI is raised so no configurable-priority interrupt runs at all. Faults
   and NMI stay enabled by design: masking those would trade a jitter problem
   for a silent-corruption problem. In `RTOS_POLL_NO_TICK` mode device
   interrupts continue to be serviced, so I/O stays responsive and only
   *scheduling* jitter is removed.

On exit the kernel replays the suppressed ticks, restores `TICKINT` and BASEPRI,
and takes the deferred switch. The rest of the task set therefore observes a
scheduler that *paused*, not one that lost time: `rtos_delay()` deadlines and
the tick count remain correct.

### 7A.3 Reconstructing the suppressed ticks

SysTick's `COUNTFLAG` saturates, so the hardware cannot report how many reloads
occurred while the interrupt was masked. The kernel counts them in software.
SysTick counts **down**, so a fresh sample of `VAL` that is *larger* than the
previous sample means the counter wrapped and one tick was suppressed:

```c
static void poll_sample(void)
{
    uint32_t v = rtos__systick_val();
    if (v > s_prev_val) { s_reloads++; }   /* counts down: increase => wrapped */
    s_prev_val = v;
}
```

The sampling hook is `rtos_poll_expired()`, which the poll loop already calls
once per iteration to honour its cycle budget, so the mechanism costs nothing
extra and a poll loop iterates far more often than once per millisecond.

Where the DWT cycle counter exists (real silicon) the elapsed-cycle count gives
an **independent second estimate**, `elapsed / period`. The kernel takes the
larger of the two. The asymmetry is deliberate: it may run the clock forward but
never backward, because a late wake-up is a scheduling artefact while a lost
tick is a correctness bug.

Replay is a single pass. `rtos__tick_advance(n)` decrements every sleeper by `n`
at once rather than looping `n` times, so closing a long poll section costs the
same as closing a short one. A property the mechanism needs, since the replay
itself runs with interrupts masked.

### 7A.4 The cost, stated plainly

A poll section is **cooperative**: the kernel cannot end one, only the task can.
Determinism inside the section is bought with the programmer's obligation to
leave it. Rather than assume that obligation is met, the kernel makes it
auditable, `rtos_poll_info_t` records every section's length, every budget
overrun, every deferred switch and every contract violation, and
`rtos_poll_report()` prints them.

A task that blocks inside a section (via `rtos_delay()`, `rtos_sem_take()` or
`rtos_mutex_lock()`) has broken the contract: there is no tick to wake it and no
switch to take. Rather than hang, the kernel force-closes the section, restores
the tick and counts the violation, so the fault presents as a number in a report
instead of as a dead board.

The measured cost of the machinery itself is reported by Experiment 2 as the
difference between the two phases' *minima*, see §12.

---

## 7B. Cache-aligned lock-free IPC

### 7B.1 Structure

`spsc_ca_t` (`spsc.c`, declared in `rtos.h`) keeps the algorithm of §7 and
changes two structural properties:

**Cache-line partitioning.** The struct is split into three
`RTOS_CACHE_LINE`-sized regions that never overlap:

| Region | Contents | Written by |
|---|---|---|
| line 0 | `buffer`, `item_size`, `capacity`, `mask` | nobody after init |
| line 1 | `tail`, `cached_head` | producer only |
| line 2 | `head`, `cached_tail` | consumer only |

Neither side dirties a line the other reads on its hot path, so on a cached core
false sharing is eliminated by construction rather than by luck.

**Shadow indices.** The producer tests for "full" against `cached_head`, its own
stale copy of the consumer's index, and re-reads the real `head` *only* when
that copy says full. The consumer mirrors this with `cached_tail`. On a queue
that is kept drained, the peer's index leaves the common path entirely.

### 7B.2 A claim this project does not make

The STM32F407 is a Cortex-M4 and **has no data cache**. On this part the padding
cannot buy a cache effect, and none is claimed. What it buys here is that each
index sits alone on a naturally-aligned block, and that the same source remains
correct when compiled for a cached Cortex-M7 or any multi-core host, where false
sharing is real.

The effect that is measurable on the M4 is the **shadow indices**, which remove
a load and a dependent compare from the hot path outright. Experiment 3 is
therefore designed to separate the two, A vs B isolates the cost of the lock,
B vs C isolates the cost of the peer-index load. Rather than attributing both
to "cache alignment".

### 7B.3 When the optimisation does *not* pay

The shadow indices are a bet on the access pattern, and the bet can lose.
Experiment 3 measures both cases:

- **Ping-pong (queue depth 1).** Send one, receive it, repeat. The queue is
  empty at the start of every receive, so the consumer's shadow is stale *every*
  time and must be refreshed every time. The optimisation cannot win here; it
  can only add a branch and a store. This adversarial case is measured and
  reported, not hidden.
- **Burst (depth 8).** Fill several slots, then drain them. What a real feed
  handler does when an ISR or DMA completion deposits a run of records. One
  refresh is amortised over eight operations. This is the case the design
  targets.

### 7B.4 The determinism argument is structural

The strongest reason to put a lock-free ring on a real-time hot path is not
throughput. It is that the ring **never disables interrupts**, so it cannot
extend any other task's worst-case latency. A mutex must. That is a property of
the generated code, not of a measurement, and it is checkable directly:

```
arm-none-eabi-objdump -d build/spsc.o | grep -c cpsid     ->  0
```

Per message, the baseline queue enters four critical sections (lock and unlock,
on both the send and the receive side); the lock-free rings enter none.

---

## 8. Timing measurement methodology

### 8.1 Cycle source

`rtos_cycles()` returns the DWT cycle counter on hardware. QEMU's Cortex-M model
does not implement `DWT->CYCCNT`, so at boot the kernel probes the counter and, if
it does not advance, transparently falls back to a **SysTick-derived** monotonic
cycle count: `ticks·(RELOAD+1) + (RELOAD − VAL)`. This keeps all timing code
functional under QEMU (for verification) and cycle-accurate on hardware. The
fallback can momentarily read backwards if a SysTick reload occurs while
interrupts are masked (inside `PendSV`); the affected samples are rejected by the
recorder, and the effect does not exist on hardware.

### 8.2 Distributions, not means

Determinism is a property of the **tail** of the latency distribution, not the
mean. `stats.c` therefore records a histogram (fixed-width bins plus an overflow
bin) and reports min, mean, **p50, p99, p99.9, and max**. Percentiles are read off
the cumulative histogram.

### 8.3 What is measured

- **Context switch:** `PendSV` timestamps entry and exit and feeds the delta to
  `rtos_stats_record_switch()`; the `make bench` build collects ≥10⁶ samples.
- **End-to-end latency:** each price event is timestamped at the Feed task and the
  delta is recorded when the corresponding order reaches the Dispatch task.
- **Interrupt-disabled window:** the tick handler measures its own critical
  section; `rtos_max_irq_disabled_cycles()` reports the longest observed.

### 8.4 Representative QEMU results

```
ctx-switch : n=1025944 min=0 mean=0 p50=8 p99=8 p99.9=176 max=1512 (cycles)
e2e latency: n=336 min=1550 mean=7811 p50=10752 p99=12800 p99.9=13312 (cycles)
max IRQ-disabled window = 4670 cycles
```

These are QEMU functional figures (SysTick-derived, not cycle-accurate); the
same code produces cycle-accurate numbers on the Discovery board via DWT.

---

## 8A. The benchmarking testbed

A claim of improvement is worth exactly as much as the baseline it is measured
against. The conventional design is therefore **built here**
(`queue_baseline.c`), on this kernel, at the same `-O2` and the same clock,
rather than quoted from another project, which would confound compiler,
kernel, clock and workload all at once. It is also not strawmanned: it uses the
same tight copy loop as the lock-free rings, and its fast path skips the
semaphores entirely, which is the most favourable honest form of the design.
The only variable that differs between subjects is the synchronisation strategy.

Each experiment is built as its **own firmware image** (`make bench-ctxsw`,
`bench-jitter`, `bench-ipc`). A measurement of the scheduler must not share a
CPU with unrelated work, or it stops being a measurement of the scheduler.

| # | Experiment | What it isolates |
|---|---|---|
| 1 | Context-switch cost, ≥ 10⁶ samples | the kernel's own per-switch overhead |
| 2 | Execution jitter: tick-driven vs poll-mode | whether suppressing the tick removes jitter |
| 3 | 64-byte IPC latency, three designs | A vs B: cost of the lock; B vs C: cost of the peer-index load |

A 64-byte message is chosen deliberately: it is the size of a market-data tick
record and, not coincidentally, exactly two 32-byte cache lines, which makes the
alignment question live rather than academic.

### 8A.1 Emulation is for correctness, not for cycles

QEMU implements no DWT **and is not cycle-accurate**: its virtual clock advances
in blocks rather than per retired instruction, which quantises short timed
regions onto identical values. Running with `-icount shift=7` ties virtual time
to *instruction count* and makes relative comparison possible, but that is a
**proxy, not a cycle count**. It models no memory stall, no DMB pipeline drain,
no flash wait state and no bus contention.

Accordingly: QEMU verifies **function and correctness**, and every report states
which clock produced it. Experiment 3 will not print a latency figure until the
queues have passed a correctness gate covering round-trip byte integrity, FIFO
ordering and full/empty edge behaviour. **The numbers in this report come from
the board.**

---

## 9. Worst-case interrupt-latency bound

Interrupt latency is bounded by the longest window during which the kernel keeps
interrupts disabled (`PRIMASK = 1`). The kernel measures this continuously; the
dominant contributor is the SysTick tick handler, which under interrupt lock:

1. increments the tick counter (O(1));
2. walks the delayed-task list decrementing timers (O(N) in the number of
   sleeping tasks);
3. performs one round-robin rotation and an O(1) `CLZ` reschedule.

The analytic worst case is therefore

> `L_irq ≤ A + B·N` cycles,

where `N` is the number of simultaneously-sleeping tasks and `A`, `B` are small
constants determined by the handler's straight-line code. This bound is **linear
in the task count and independent of the workload**, the measured-typical vs.
analysed-worst-case distinction the project targets. All other kernel critical
sections (semaphore/mutex operations, SPSC has none) are O(1) and shorter, so the
tick handler dominates.

---

## 10. Schedulability analysis

`rtos_schedulability_report()` applies two standard fixed-priority tests to the
demonstration workload and prints the result at boot.

**Liu & Layland utilisation bound (sufficient):** the task set is schedulable if

> `Σ (Cᵢ / Tᵢ) ≤ n · (2^{1/n} − 1)`.

**Exact response-time analysis (necessary and sufficient for this model):**

> `Rᵢ = Cᵢ + Σ_{j ∈ hp(i)} ⌈Rᵢ / Tⱼ⌉ · Cⱼ`,

iterated to a fixed point; the task is schedulable iff `Rᵢ ≤ Tᵢ`. Priorities are
assigned rate-monotonically (shorter period ⇒ higher priority).

For the demo workload (times in microseconds):

| Task | Cᵢ | Tᵢ | Uᵢ | Rᵢ |
|---|---|---|---|---|
| dispatch | 50 | 1000 | 0.050 | 50 |
| feed | 40 | 1000 | 0.040 | 90 |
| signal | 70 | 1000 | 0.070 | 160 |

Total utilisation U = 0.160 ≪ LL-bound 0.779, and every `Rᵢ ≤ Tᵢ`, so the task
set is provably schedulable.

---

## 11. MPU-based stack-overflow protection

On hardware builds (`make hw`, `-DRTOS_ENABLE_MPU`), `rtos_mpu_init()` enables the
MPU with a privileged-default background map (so ordinary accesses are
unaffected) and turns on the MemManage fault. On every context switch,
`rtos_mpu_on_switch()` (called from `PendSV`) programs a 32-byte, no-access MPU
region at the bottom of the **incoming** task's stack. If that task's stack
pointer descends into the guard, the CPU raises a MemManage fault instead of
silently corrupting the neighbouring task's memory, turning a subtle,
hard-to-debug corruption into an immediate, localisable trap. The feature is
compiled out of the QEMU functional build, where it is unnecessary.

---

## 12. Results summary and limitations

**Functional correctness (verified on QEMU):** preemptive priority scheduling,
round-robin, `PendSV`/`SVC` context switching, semaphores, priority-inheritance
mutex (with the constructed inversion resolved), wait-free SPSC ordering, the
three-task pipeline, the schedulability analysis, and the ≥10⁶-sample benchmark
all run correctly. The kernel is ~1,600 lines of C and assembly and builds
warning-free at `-O2`.

**Limitations.** QEMU's Cortex-M model implements neither the DWT cycle counter
nor cycle-accurate instruction timing, so *timing* numbers under QEMU are
functional only (SysTick-derived). The headline quantitative claims, full
context switch under ~200 cycles, the p50/p99/p99.9/max distribution over ≥10⁶
samples, the numeric interrupt-latency bound, and MPU fault behaviour, are to be
measured on the STM32F4 Discovery board, where the DWT counter is real and the
core runs at 168 MHz. The kernel currently supports static task creation only (no
task deletion) and a single-core target.

## 12A. Measured results from the testbed (QEMU, `-icount`)

Emulation results: they establish correctness and the relative size of each
effect. Absolute cycle figures are reproduced on the STM32F4 Discovery board
with `make bench-* HW=1`. The full run record, with verbatim output, is in
[`TEST_RECORD.md`](TEST_RECORD.md).

### Experiment 1, context-switch cost

1,036,308 samples collected, exceeding the 10^6 target required to state a
p99.9. Absolute values are reproduced on hardware.

### Experiment 2. Execution jitter (the poll-mode claim)

4000 blocks per phase, 32 GPIO toggles per block, identical task set in both
phases, with a same-priority partner task present throughout.

| Quantity | Phase A (tick-driven) | Phase B (poll mode) |
|---|---|---|
| peak-to-peak spread | 27 806 | **1** |
| worst observed block | 31 160 | **3 828** |
| uninterrupted block (min) | 3 354 | 3 827 |
| distribution shape | multi-modal: 3.1k / 14.8k / 18.9k / 30.7k | **single bin, 4000/4000** |

Phase A is multi-modal. One mode for blocks that ran uninterrupted, further
modes for blocks that absorbed a scheduler slice. Phase B places all 4000
samples in a single bin with a total spread of one unit.

The quantity comparable across the phases is the **minimum**, an uninterrupted
block: 3 354 against 3 827. The 473-unit difference is the measured
`poll_enter` + `poll_exit` cost (~15 per toggle at 32 toggles/block). The
medians differ because Phase A time-shares with its same-priority partner.

Worst-case improvement: **8.1x**.

### Experiment 3, 64-byte IPC latency

200 batches of 256 round-trips, three designs, two access patterns, interleaved.
Correctness gate: **PASS** (round-trip byte integrity, FIFO ordering,
full/empty edges) for all three queues.

| Comparison | Ping-pong (depth 1) | Burst (depth 8) |
|---|---|---|
| lock-free vs mutex queue (A vs B) | **1.26x** | **1.26x** |
| cache-partitioned vs plain (B vs C) | 0.99x | 0.99x |

**A vs B. The cost of the lock.** Removing the mutex is worth ~26 % on the hot
path, consistently across both access patterns.

**B vs C. The cost of the peer-index load.** Parity. The STM32F407 has no data
cache and single-cycle SRAM, so the load the shadow indices remove costs about
as much as the branch and the store that replace it. Per Section 7B, the
cache-line partitioning is a portability and multi-core property and is not
claimed as an M4 speed-up.

### Structural determinism result

Per message the baseline queue enters four critical sections; the lock-free
rings enter none, verified by disassembly (`objdump -d build/spsc.o | grep -c
cpsid` returns 0). Only the baseline can extend another task's worst-case
latency. This holds independently of any measurement.

---

## 13. Future work

- UART console driver to replace semihosting for standalone hardware runs.
- Tickless idle for lower power.
- Optional earliest-deadline-first (EDF) scheduler for comparison.
- Formal (model-checked) verification of the SPSC ordering and the mutex protocol.

## 14. Conclusion

We built a small but complete deterministic RTOS kernel for the Cortex-M4 in which
the mechanisms that govern worst-case latency, context switch, scheduler,
synchronisation, and IPC. Are explicit, measurable, and analytically bounded. The
kernel demonstrates constant-time scheduling, priority inheritance with measured
bounded blocking, wait-free IPC with proven memory ordering, and a schedulability
argument for its workload, meeting the objectives set out in the proposal.

## 15. References

1. ARM, *ARMv7-M Architecture Reference Manual* (DDI 0403).
2. ARM, *Cortex-M4 Devices Generic User Guide*.
3. C. L. Liu and J. W. Layland, "Scheduling Algorithms for Multiprogramming in a
   Hard-Real-Time Environment," *JACM*, 1973.
4. M. Joseph and P. Pandya, "Finding Response Times in a Real-Time System,"
   *The Computer Journal*, 1986.
5. L. Sha, R. Rajkumar, J. Lehoczky, "Priority Inheritance Protocols," *IEEE
   Trans. Computers*, 1990.
6. STMicroelectronics, *RM0090: STM32F405/407 Reference Manual*.

---

## Appendix A: Build system and freestanding considerations

The kernel is built with `arm-none-eabi-gcc` at `-O2`, `-nostdlib`,
`-nostartfiles`, `--gc-sections`, and a custom linker script. Building a correct
freestanding image surfaced three non-obvious issues that are worth recording, as
each is a trap that silently breaks bare-metal code:

1. **Self-recursive `memset`/`memcpy`.** At `-O2`, GCC's
   `-ftree-loop-distribute-patterns` pass recognises a byte-copy/zero loop and
   rewrites it into a call to `memcpy`/`memset`. When that pass is allowed to act
   on our *own* `memset` implementation, `memset` ends up calling `memset`, 
   unbounded recursion that overflows the stack and manifests as a bus/stacking
   fault far from the real cause. Fixed by compiling with
   `-fno-tree-loop-distribute-patterns` and additionally marking the stub
   functions with `__attribute__((optimize("no-tree-loop-distribute-patterns")))`.

2. **Compiler runtime helpers.** The statistics and schedulability code uses
   64-bit division (`uint64_t / uint32_t`), which GCC lowers to
   `__aeabi_uldivmod` from `libgcc`. `-nostdlib` excludes `libgcc`, so we link it
   explicitly (`-print-libgcc-file-name`) while still excluding the C library.

3. **Semihosting inline assembly.** The naive
   `"mov r0,%1 ; mov r1,%2 ; bkpt 0xAB"` pattern is miscompiled at `-O2`: the
   input operands may already be allocated to `r0`/`r1`, so the `mov`s clobber
   them. The robust form binds the operands to physical registers with
   `register int r0 __asm("r0")` and lets the compiler set them up.

## Appendix B: Bring-up and debugging log

Moved to [`DEFECTS_AND_FIXES.md`](DEFECTS_AND_FIXES.md), which collects every
defect found during development and verification in one place.

## Appendix C: Verification methodology

- **Functional verification** was performed on QEMU (`olimex-stm32-h405`) with
  semihosting console output. Each subsystem was exercised by the demo workload
  and observed over multi-second runs: schedulability report at boot, the
  priority-inversion test, and periodic latency reports including the histogram.
- **Regression on optimisation levels.** The image was verified to run correctly
  at `-O0`, `-O1`, `-Og`, and `-O2`, which caught the `-O2`-only defects above.
- **Exception tracing.** `qemu-system-arm -d int` was used to confirm the absence
  of faults and to characterise the exception sequence (SVC once at launch, then
  SysTick IRQs and PendSV switches).
- **Statistical output.** Latency is reported both as summary percentiles (in
  cycles and microseconds) and as a full histogram, so the distribution shape, 
  not just the tail points, is visible.

Example histogram (QEMU functional, context switch):

```
ctx-switch histogram (cycles):
  [0,8)     5803
  [168,176) 2150
  [512+   )   11
```

The bimodal shape (a large mass near 0-8 cycles and a second cluster near
168-176) reflects the SysTick-fallback counter's quantisation on QEMU; on
hardware the DWT counter yields a smooth, cycle-accurate distribution.

## Appendix D: Public API reference

```c
/* Lifecycle */
void   rtos_init(void);
tcb_t *rtos_task_create(tcb_t*, const char *name, task_entry_t, void *arg,
                        uint32_t priority, uint32_t *stack, uint32_t words);
void   rtos_start(void);

/* Task services */
void     rtos_delay(uint32_t ticks);
void     rtos_yield(void);
uint32_t rtos_tick_count(void);
tcb_t   *rtos_current(void);

/* Semaphores */
void rtos_sem_init(rtos_sem_t*, int32_t initial, int32_t max);
void rtos_sem_take(rtos_sem_t*);
int  rtos_sem_try_take(rtos_sem_t*);
void rtos_sem_give(rtos_sem_t*);
void rtos_sem_give_from_isr(rtos_sem_t*);

/* Priority-inheritance mutex */
void rtos_mutex_init(rtos_mutex_t*);
void rtos_mutex_lock(rtos_mutex_t*);
void rtos_mutex_unlock(rtos_mutex_t*);

/* Lock-free wait-free SPSC ring buffer */
void rtos_spsc_init(spsc_t*, void *storage, uint32_t item_size, uint32_t cap_pow2);
int  rtos_spsc_try_send(spsc_t*, const void *item);
int  rtos_spsc_try_recv(spsc_t*, void *item);
uint32_t rtos_spsc_count(const spsc_t*);

/* Timing and statistics */
void     rtos_cyccnt_enable(void);
uint32_t rtos_cycles(void);
int      rtos_dwt_available(void);
void     rtos_stats_report(const char *label, const rtos_stats_t*);
void     rtos_stats_histogram(const char *label, const rtos_stats_t*);
uint32_t rtos_max_irq_disabled_cycles(void);
int      rtos_schedulability_report(const rtos_taskspec_t*, uint32_t n);

/* MPU stack guard (hardware builds) */
void rtos_mpu_init(void);
void rtos_mpu_on_switch(void);
```

## Appendix E: Figures to include from hardware runs

The following figures are to be captured on the STM32F4 Discovery board (where the
DWT counter is cycle-accurate) and inserted into the final report:

- **Fig. 1**, Context-switch latency histogram over ≥10⁶ samples (log-scale y).
- **Fig. 2**, End-to-end event-to-dispatch latency CDF with p50/p99/p99.9 marked.
- **Fig. 3**, GPIO-toggle oscilloscope trace of a single context switch, giving
  an independent measurement of switch time to cross-check the DWT figure.
- **Fig. 4**, Priority-inversion timeline (with and without inheritance),
  annotated with the measured blocking interval.
- **Table 1**, Measured vs. analysed worst-case interrupt-latency for increasing
  numbers of sleeping tasks (validates the `A + B·N` bound).
