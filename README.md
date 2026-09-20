# Micro-Trade OS

**A deterministic, low-latency RTOS kernel for the ARM Cortex-M4 (STM32F407),
built on a bare-metal boot environment written from scratch.**

Measured head to head against FreeRTOS on the same hardware model, the same boot
layer and the same workload.

---

## What this repository is

The repository has three directories. The RTOS kernel boots on the environment
built by the boot layer, and the FreeRTOS baseline is the control it is measured
against.

Nothing here depends on a vendor HAL, on CMSIS, or on a C standard library.
Every layer from the reset vector upward is in this repository.

| Directory | What it is | Why it is here |
|---|---|---|
| [`baremetal_boot/`](baremetal_boot/) | The bare-metal startup code: Cortex-M4 boot from reset to `main()`, covering the vector table, `.data`/`.bss` runtime initialisation, the SysTick timer, and console output over semihosting | The **foundation**. It establishes and verifies the boot contract (correct initial SP, correct `Reset_Handler`, initialised memory, a working interrupt) that the RTOS assumes on entry. Kept because the RTOS's claims start from a verified reset, not an assumed one. |
| [`rtos_kernel/`](rtos_kernel/) | The RTOS itself: a preemptive fixed-priority kernel with poll-mode scheduling, cache-partitioned lock-free message passing, and a benchmarking testbed | The **main work**. See [`rtos_kernel/docs/Project_Objectives.pdf`](rtos_kernel/docs/Project_Objectives.pdf) for the objectives, the method, and the results. |
| [`freertos_baseline/`](freertos_baseline/) | FreeRTOS V11.1.0 running the same experiments, on the same boot layer, with the same measurement code | The **control**. The objective is stated against FreeRTOS, so FreeRTOS has to be measured rather than cited. Head-to-head figures: [`COMPARISON_FREERTOS.md`](rtos_kernel/docs/COMPARISON_FREERTOS.md). |

`rtos_kernel/` carries its own `startup/` and `ld/` so it builds standalone.
They are the `baremetal_boot/` files, extended with the `SVC` and `PendSV`
vectors the kernel needs.

## Quick start

All three build with the GNU Arm Embedded toolchain and run under QEMU:

```bash
brew install --cask gcc-arm-embedded && brew install qemu   # macOS
# sudo apt install gcc-arm-none-eabi qemu-system-arm        # Ubuntu
```

**The boot sequence**

```bash
cd baremetal_boot && make && make qemu
```

Prints the boot banner and a SysTick interrupt heartbeat. Exit with `Ctrl-A` then `X`.

**The RTOS**

```bash
cd rtos_kernel && make && make qemu
```

Prints the schedulability analysis, the priority-inversion result, and periodic
latency reports. The three benchmark experiments are separate images. Build them
with `make bench-jitter`, `make bench-ipc` or `make bench-ctxsw`. That
directory's README explains how to run them, and why QEMU results need
`-icount`.

**The FreeRTOS baseline**

```bash
cd freertos_baseline && make bench-jitter && make qemu
```

Runs the same experiments on FreeRTOS V11.1.0, so the comparison is measured
rather than cited.

## Verification

| Document | Contents |
|---|---|
| [`docs/TEST_RECORD.md`](rtos_kernel/docs/TEST_RECORD.md) | Results of an actual QEMU run of every build and every experiment: commands and verbatim output |
| [`docs/DEFECTS_AND_FIXES.md`](rtos_kernel/docs/DEFECTS_AND_FIXES.md) | Defects found and fixed during development, with root cause and confirmation |
| [`docs/OUTSTANDING_WORK.md`](rtos_kernel/docs/OUTSTANDING_WORK.md) | What remains: hardware runs, captures, and work left out of scope |

## A note on the numbers

QEMU verifies **function and correctness**. It implements no DWT cycle counter
and is not cycle-accurate, so it cannot produce the timing figures the report
needs. Every report states which clock produced it, and the figures for the
final report come from the STM32F4 Discovery board (`make bench-* HW=1`).

## License

MIT, see [`rtos_kernel/LICENSE`](rtos_kernel/LICENSE).
