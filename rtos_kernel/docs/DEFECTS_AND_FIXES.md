# Defects Found and Fixed

Engineering log of defects found during development and verification. It is
kept separate from the results documents, so those report measurements only.

Each entry records the symptom, the root cause, the fix, and how the fix was
confirmed.

One thing links all of them. None announced itself. Every one produced
confident-looking output rather than an obvious failure.

---

## D1: Build system silently linked the wrong image

**Symptom.** Running plain `make` immediately after `make bench-ctxsw` produced
a 6,404-byte image. The benchmark, not the demo. The build succeeded and the
image ran; it was simply not the program under test.

**Root cause.** The four build modes differ only in `-D` flags over the *same*
sources. After a benchmark build every `.o` was newer than its `.c`, so Make
rebuilt nothing and relinked the benchmark objects into what looked like a
demo image.

**Fix.** A flag stamp file, `build/.buildflags`, which records the defines the
objects were compiled with and which every object depends on. A change of
defines now forces a full rebuild.

**Confirmed.**
```
after bench-ctxsw: 6404 bytes text
[FLAGS] -DRTOS_CPU_MHZ=8
after plain make:  9560 bytes text   <- correct demo image
```

---

## D2: Histogram saturation reported as a valid percentile

**Symptom.** In an early run of Experiment 3, all six subjects reported an
identical per-message figure. Read literally, three different queue designs
measured by two access patterns had produced exactly the same number.

**Root cause.** `rtos_stats_percentile()` returned the upper edge of the bin
containing the percentile. The overflow bin has no upper edge, so once the data
exceeded `RTOS_STAT_BINS × bin_width` every subject returned the same
fabricated value `(RTOS_STAT_BINS + 1) × bin_width`. The designs were not
identical; the instrument was out of range.

**Fix.** Three parts:
1. The percentile now returns the true `max` when the percentile falls in the
   overflow bin, rather than a fictitious edge.
2. `rtos_stats_overflow()` exposes the count of out-of-range samples, and
   `rtos_stats_report()` prints an explicit warning naming that count and the
   bin width whenever it is non-zero.
3. Bin widths are chosen from each experiment's expected range rather than by
   habit, and per-message rather than per-batch values are recorded so they
   fall inside it.

**Confirmed.** The guard fires correctly in Experiment 1, which legitimately
has a long tail: `*** 150 of 1036308 samples exceeded the histogram range ***`.
Experiments 2 and 3 now run without warnings.

---

## D3: Measurement clock stalled inside poll sections

**Symptom.** Timed regions inside poll sections all measured near-identical,
near-zero durations, making every design look the same.

**Root cause.** On the QEMU fallback path `rtos_cycles()` derives time from the
tick count, and a poll section is *defined* by the tick interrupt being
masked. The clock therefore stops for exactly the region it was being asked to
measure.

**Fix.** `poll.c` maintains its own reload-counting clock, valid inside a
section, and the benchmarks select between the two via `bench_now()`. On
hardware both resolve to `DWT->CYCCNT` and the distinction costs one
predictable branch.

---

## D4: Worst-case interrupts-disabled window off by a full tick

**Symptom.** The demo reported a worst-case interrupts-disabled window of
~7,990 cycles (≈1 ms) for a handler that plainly cannot take a millisecond.

**Root cause.** Pre-existing, and independent of poll mode. The tick handler
sampled `rtos_cycles()` before and after `rtos__tick_advance()`, the very call
that increments `g_ticks`. While `rtos_cycles()` itself contains a
`g_ticks × period` term. Every sample therefore carried one spurious tick. The
existing `dt < RELOAD` filter caught only some of them.

**Fix.** The measurement now uses a raw sub-tick clock with no tick-count term
(`raw_ticker()` / `raw_delta()`): `DWT->CYCCNT` on silicon, SysTick's own `VAL`
otherwise. Where the fallback sample is ambiguous, `VAL` apparently increasing,
which on non-cycle-accurate QEMU may be either a genuine reload or emulator
jitter. The sample is discarded rather than guessed at, which is conservative
on QEMU and exact on hardware.

Because QEMU cannot support the claim either way, the demo now prints
`n/a (needs DWT; QEMU is not cycle-accurate)` instead of an emulator artefact.

---

## D5: A stated prediction contradicted by the data

**Symptom.** Experiment 2 was written to predict that the two phases' *medians*
must match, on the reasoning that poll mode removes interference without making
the loop faster. The measured medians were 15,360 and 4,096.

**Root cause.** The prediction was wrong, not the measurement. Phase A shares
the CPU with a same-priority partner task, so a majority of its blocks
legitimately contain a slice of that partner. That time-sharing is the baseline
behaviour under test, not an error term.

**Fix.** The comparable quantity is the *minimum*, meaning an uninterrupted block. The
experiment, its reporter, the README and the objectives PDF were all corrected
to state this. The gap between the two minima is now reported as the measured
cost of the poll machinery, at 473 units per block. It is no longer absorbed
into the result.

---

## D6: Fragile sleeper-list walk in the tick handler

**Symptom.** None observed; found while extending the tick path for poll-mode
tick replay.

**Root cause.** The original delayed-list walk could exit early after waking a
task, leaving the remaining sleepers un-decremented for that tick.

**Fix.** Replaced with `rtos__tick_advance(n)`, which counts the list first and
uses that count as the loop bound, saving each node's successor before the node
can be unlinked. It also handles `n > 1` in a single pass, which poll-mode
replay requires: closing a long poll section costs the same as closing a short
one, and the replay runs with interrupts masked.

---

## Earlier defects, found during initial QEMU bring-up

Three defects predating the poll-mode and lock-free-IPC work, all classic
Cortex-M RTOS pitfalls. QEMU's `-d int` exception trace and `arm-none-eabi-gdb`
over the QEMU gdbstub were the primary tools.

### D7: HardFault on first task launch

`rtos_start()` pended `PendSV` before the first task existed. With `PendSV`
enabled and `PRIMASK` clear it fired immediately, while `rtos_current_task` was
still `NULL` and the PSP uninitialised. Corrupting the first switch and
skipping SysTick setup.

**Fix.** Launch the first task exclusively through `SVC`, selecting it without
pending `PendSV`.

### D8: `SVC` escalating to HardFault

Issuing the launch `SVC` with interrupts masked (`PRIMASK=1`) caused the
synchronous `SVC` exception to escalate to a HardFault.

**Fix.** Keep interrupts enabled across the launch `SVC`.

### D9: `memset` recursion at `-O2`

At `-O2`, GCC's loop-distribute-patterns pass rewrote the byte loop inside
`memset()` into a call to `memset()`. Unbounded recursion. Observed as a
`BusFault` with `BFSR.STKERR` at `0x1ffffff8`, just below RAM: the signature of
a stack overflow from runaway recursion.

**Fix.** The per-function `optimize("no-tree-loop-distribute-patterns")`
attribute on the freestanding `memset`/`memcpy` in `libc_stubs.c`.

**Note.** Building at `-O0`, `-O1`, `-Og` and `-O2` is itself part of the
verification procedure; it is what exposed this class of defect.
