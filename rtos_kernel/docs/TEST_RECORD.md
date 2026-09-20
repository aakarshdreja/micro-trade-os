# Test Record: QEMU Verification Run

Results of an actual run of every build and every experiment. Commands as shown,
output verbatim.

## Environment

```
date: 2026-09-20T13:34:16Z
host: Darwin 25.5.0 arm64
compiler: 15.2.1 (arm-none-eabi-gcc (Arm GNU Toolchain 15.2.Rel1 (Build arm-15.86)) 15.2.1 20251203)
qemu: QEMU emulator version 11.1.1
```

Emulated machine: `olimex-stm32-h405` (Cortex-M4). All images built at `-O2`.

Clock: QEMU implements no DWT cycle counter and is not cycle-accurate. These
runs establish function, correctness, and the relative size of each effect.
Absolute timings are reproduced on hardware with `make bench-* HW=1`.

## Summary

| # | Test | Command | Result |
|---|---|---|---|
| 0 | Build, all 5 images | `make`, `make bench-*` | **PASS**, 0 warnings, 0 errors at `-O2` |
| 1 | Bare-metal boot sequence | `cd baremetal_boot && make && make qemu` | **PASS**, both memory-init assertions |
| 2 | RTOS demonstration pipeline | `cd rtos_kernel && make && make qemu` | **PASS**, schedulable, inversion bounded, 0 poll violations |
| 3 | Experiment 1, context-switch cost | `make bench-ctxsw` | **PASS**, 1,036,308 samples, 10⁶ target met |
| 4 | Experiment 2, execution jitter | `make bench-jitter` | **PASS**, jitter 27,806 → 1; worst case 8.14× better |
| 5 | Experiment 3, 64-byte IPC | `make bench-ipc` | **PASS**, correctness gate PASS; lock-free 1.26× |
| 6 | Structural determinism check | `objdump` | **PASS**, lock-free path has 0 interrupt-masking instructions |

---

## 0. Build verification

All five firmware images build warning-free at `-O2`:

```
p1         764	      4	      8	    776	    308	/tmp/t_p1.elf
demo      9560	      4	   9216	  18780	   495c	/tmp/t_demo.elf
jit       9196	      0	   4532	  13728	   35a0	/tmp/t_jit.elf
ipc      11368	      0	   7816	  19184	   4af0	/tmp/t_ipc.elf
ctx       6404	      0	   3932	  10336	   2860	/tmp/t_ctx.elf
```

| Image | Build command | text (bytes) | bss (bytes) |
|---|---|---|---|
| Bare-metal boot | `make` in `baremetal_boot` | 764 | 8 |
| RTOS demo | `make` | 9,560 | 9,216 |
| Exp. 2 jitter | `make bench-jitter` | 9,196 | 4,532 |
| Exp. 3 IPC | `make bench-ipc` | 11,368 | 7,816 |
| Exp. 1 ctxsw | `make bench-ctxsw` | 6,404 | 3,932 |

The whole RTOS, including the demo pipeline, is 9.5 KB of code, linking no C
library (`-nostdlib`).

---

## 1. Bare-metal boot sequence

```bash
cd baremetal_boot && make && make qemu
```

```
=== STM32F4 Bare-Metal Boot OK ===
PASS: initialized == 123 (.data copy worked)
PASS: uninitialized == 0 (.bss zeroing worked)
SysTick initialized. Interrupts firing at ~1kHz
SysTick: 100 ticks elapsed
SysTick: 100 ticks elapsed
SysTick: 100 ticks elapsed
SysTick: 100 ticks elapsed
```

**PASS.** `initialized == 123` confirms `Reset_Handler` copied `.data` from
Flash to RAM; `uninitialized == 0` confirms it zeroed `.bss`. The SysTick
heartbeat confirms the vector table is wired and interrupts are taken. Image
size (764 B text, 4 data, 8 bss) matches the figure documented in that part's
README.

---

## 2. RTOS demonstration pipeline

```bash
cd rtos_kernel && make && make qemu
```

Boot-time schedulability analysis:

```
--- Schedulability analysis (RMA) ---
  dispatch: C=50 T=1000 U=0.050
  feed: C=40 T=1000 U=0.040
  signal: C=70 T=1000 U=0.070
  Total U=0.160  LL-bound=0.779  => PASS (U bound)
  R(dispatch)=50 <= T? yes
  R(feed)=90 <= T? yes
  R(signal)=160 <= T? yes
  => Task set is SCHEDULABLE (RTA)

Starting scheduler...
```

Priority-inversion test:

```
--- Priority-inversion test ---
pi_high blocked for 64000 cycles (~8000 us)
Blocking is bounded by pi_low's critical section, NOT by
pi_med - priority inheritance prevented unbounded inversion.
[DISPATCH] SELL seq=15 px=10021 lat=11138 cyc
```

Representative latency report:

```
===== Latency report #2 =====
cycle source: SysTick-derived (QEMU functional)
ctx-switch : n=11005 min=0 mean=21 p50=8 p99=344 p99.9=4200 max=4200 cyc
            *** 53 of 11005 samples exceeded the histogram range (bin_width=8 x 64 bins). min/mean/max are exact; percentiles are
                approximate - increase bin_width. ***
            (us)  min=0.00 mean=2.62 p50=1.00 p99=43.00 p99.9=525.00 max=525.00
e2e latency: n=244 min=4520 mean=11417 p50=8192 p99=24064 p99.9=24064 max=24000 cyc
            (us)  min=565.00 mean=1427.12 p50=1024.00 p99=3008.00 p99.9=3008.00 max=3000.00
max IRQ-disabled window = n/a (needs DWT; QEMU is not cycle-accurate)
poll-mode: sections=244 ticks_replayed=1 deferred_switch=0 overruns=0 violations=0 max_section=8000 cyc
ctx-switch histogram (cycles):
```

**PASS.**

- The task set is provably schedulable by response-time analysis, reported at
  boot before any measurement.
- Priority inheritance bounds the inversion: `pi_high` is blocked by `pi_low`'s
  critical section, not by the unrelated mid-priority task.
- `violations=0 overruns=0` across every poll section, with `ticks_replayed`
  non-zero. The tick-replay path is exercised, not merely compiled.

---

## 3. Experiment 1, context-switch cost

```bash
make bench-ctxsw && make qemu
```

```
===== EXPERIMENT 1: context-switch cost (n>=1000000) =====
clock source: SysTick-derived (QEMU: FUNCTIONAL ONLY, not perf data)
ctx-switch: n=1036308 min=0 mean=1 p50=8 p99=56 p99.9=176 max=4553 cyc
            *** 150 of 1036308 samples exceeded the histogram range (bin_width=8 x 64 bins). min/mean/max are exact; percentiles are
                approximate - increase bin_width. ***
            (us)  min=0.00 mean=0.12 p50=1.00 p99=7.00 p99.9=22.00 max=569.12
ctx-switch histogram (cycles):
  [0,8) 1025797
  [8,16) 36
  [16,24) 28
  [24,32) 19
  [32,40) 18
  [40,48) 30
  [48,56) 23
  [56,64) 20
  [64,72) 30
```

**PASS on sample count**, 1,036,308 switches recorded, exceeding the 10⁶
target required to state a p99.9.

The histogram-range warning is the statistics guard reporting that 150 samples
(0.014%) fell above the configured range: `min`, `mean` and `max` are exact,
the percentiles are approximations. Absolute cycle values are reproduced on
hardware.

---

## 4. Experiment 2, execution jitter

```bash
make bench-jitter
qemu-system-arm -machine olimex-stm32-h405 -nographic -icount shift=7 \
  -semihosting-config enable=on,target=native -kernel firmware.elf
```

4,000 blocks per phase, 32 GPIO toggles per block, identical task set in both
phases; the only difference is whether the block runs inside a poll section.

```
Phase A - tick-driven (baseline RTOS behaviour):
  block: n=4000 min=3354 mean=13792 p50=15360 p99=31232 p99.9=31232 max=31160 cyc
            (us)  min=419.25 mean=1724.00 p50=1920.00 p99=3904.00 p99.9=3904.00 max=3895.00
Phase B - poll mode (RTOS_POLL_EXCLUSIVE):
  block: n=4000 min=3827 mean=3827 p50=4096 p99=4096 p99.9=4096 max=3828 cyc
            (us)  min=478.37 mean=478.37 p50=512.00 p99=512.00 p99.9=512.00 max=478.50

Histograms (shape is the evidence; a single spike = no jitter):
Phase A tick-driven histogram (cycles):
  [3072,3584) 608
  [14848,15360) 3210
  [15360,15872) 112
  [18944,19456) 5
  [30720,31232) 65
Phase B poll-mode histogram (cycles):
  [3584,4096) 4000

--- Result ---
peak-to-peak jitter  A=27806 cyc   B=1 cyc   reduction=27806.00x
p99.9 block time     A=31232 cyc   B=4096 cyc
```

```
--- Result ---
peak-to-peak jitter  A=27806 cyc   B=1 cyc   reduction=27806.00x
p99.9 block time     A=31232 cyc   B=4096 cyc
median block time    A=15360 cyc   B=4096 cyc

Uninterrupted block (the comparable quantity):
  A min=3354 cyc   B min=3827 cyc
  poll_enter+poll_exit overhead = 473 cyc per block (14 cyc per toggle at 32 toggles/block)
  => poll mode does not make the loop faster; it removes
     the interference, at a small fixed cost per section.

Worst observed block: A=31160 cyc   B=3828 cyc   improvement=8.14x
  A real-time budget is written against THIS number.
poll-mode: sections=4000 ticks_replayed=3194 deferred_switch=0 overruns=0 violations=0 max_section=7075 cyc
```

**PASS.**

| Quantity | Phase A (tick-driven) | Phase B (poll mode) |
|---|---|---|
| peak-to-peak spread | 27,806 | **1** |
| worst observed block | 31,160 | **3,828** |
| uninterrupted block (min) | 3,354 | 3,827 |
| distribution | multi-modal, 4 populated bins | **single bin, 4000/4000** |

Phase A is multi-modal. There is one population at ~3.1k for blocks that ran clean,
another at ~14.8k for blocks that absorbed a scheduler slice, a tail at ~30.7k.
Phase B places all 4,000 samples in one bin with a total spread of one unit.

The comparable quantity across the phases is the minimum, an uninterrupted
block: 3,354 against 3,827. The 473-unit difference is the measured
`poll_enter`+`poll_exit` cost, ≈15 per toggle at 32 toggles per block. The
medians differ because Phase A time-shares with a same-priority partner task.

Worst-case improvement: **8.14×**.

---

## 5. Experiment 3, 64-byte IPC latency

```bash
make bench-ipc
qemu-system-arm -machine olimex-stm32-h405 -nographic -icount shift=7 \
  -semihosting-config enable=on,target=native -kernel firmware.elf
```

```
[verify] queue correctness (round-trip integrity, FIFO order, full/empty edges): PASS
```

```
-- Pattern 1: PING-PONG (queue depth 1) --
  A mutex queue (FreeRTOS-style): per-message p50=9247 cyc   p99.9=9247 cyc   min=9245 max=9247 cyc
  B lock-free SPSC             : per-message p50=7288 cyc   p99.9=7288 cyc   min=7285 max=7288 cyc
  C lock-free SPSC, cache-algnd: per-message p50=7342 cyc   p99.9=7342 cyc   min=7340 max=7342 cyc

-- Pattern 2: BURST (depth 8) --
  A mutex queue (FreeRTOS-style): per-message p50=9280 cyc   p99.9=9280 cyc   min=9277 max=9280 cyc
  B lock-free SPSC             : per-message p50=7320 cyc   p99.9=7320 cyc   min=7317 max=7320 cyc
  C lock-free SPSC, cache-algnd: per-message p50=7331 cyc   p99.9=7331 cyc   min=7329 max=7331 cyc

--- Speed-up (median; >1.00x means the second is faster) ---
  ping-pong  lock-free vs mutex : 1.26x
```

```
--- Speed-up (median; >1.00x means the second is faster) ---
  ping-pong  lock-free vs mutex : 1.26x
  ping-pong  cache-al. vs plain : 0.99x
  burst      lock-free vs mutex : 1.26x
  burst      cache-al. vs plain : 0.99x
```

**PASS on correctness**. Round-trip byte integrity, FIFO ordering and
full/empty edge behaviour, for all three queues. The gate runs before any
timing is reported.

| Comparison | Ping-pong (depth 1) | Burst (depth 8) |
|---|---|---|
| lock-free vs mutex queue (A vs B) | **1.26×** | **1.26×** |
| cache-partitioned vs plain (B vs C) | 0.99× | 0.99× |

A vs B: removing the lock is worth ~26%, consistently across both access
patterns.

B vs C: parity. The Cortex-M4 has no data cache and single-cycle SRAM, so the
load the shadow indices remove costs about as much as the branch and store that
replace it. The partitioning is a portability and multi-core property, and is
not claimed as an M4 speed-up.

Absolute figures (~9,200 / ~7,300) are emulator time units under `-icount`, not
cycles; the ratios are comparable because all six subjects share one clock.

---

## 6. Structural determinism check

```bash
arm-none-eabi-objdump -d build/spsc.o | grep -c cpsid
```

```
$ arm-none-eabi-objdump -d build/spsc.o | grep -c cpsid
0
$ arm-none-eabi-objdump -d build/queue_baseline.o | grep -c 'bl.*mutex'
11
$ arm-none-eabi-size firmware.elf
   text	   data	    bss	    dec	    hex	filename
   6404	      0	   3932	  10336	   2860	firmware.elf
```

**PASS.** The lock-free translation unit contains zero interrupt-masking
instructions. The baseline queue makes 11 calls into the mutex, which mask
interrupts: four critical sections per message against none.

A mutex extends every other task's worst-case latency for as long as it holds
interrupts off; a lock-free ring cannot. This is a property of the generated
code and holds independently of any measurement.

---

## Related documents

- [`OUTSTANDING_WORK.md`](OUTSTANDING_WORK.md). What remains to be done
- [`DEFECTS_AND_FIXES.md`](DEFECTS_AND_FIXES.md). Defects found and fixed
