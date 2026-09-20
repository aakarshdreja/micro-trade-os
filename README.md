# Micro-Trade OS

**A deterministic, low-latency RTOS kernel for the ARM Cortex-M4 (STM32F407),
built on a bare-metal boot environment written from scratch.**

Aakarsh D Reja (2023EE11151) · Jayesh Narayanan (2023EE11048)
Department of Electrical Engineering, IIT Delhi

---

## What this repository is

The project has two parts, and the numbering is the dependency order: the RTOS
in Part 2 boots on the environment built in Part 1. Nothing here depends on a
vendor HAL, on CMSIS, or on a C standard library. Every layer from the reset
vector upward is in this repository.

| | Directory | What it is | Why it is here |
|---|---|---|---|
| 1 | [`baremetal_boot/`](baremetal_boot/) | ELL365 assignment: Cortex-M4 startup from reset to `main()`, vector table, `.data`/`.bss` runtime init, SysTick, semihosting | The **foundation**. It establishes and verifies the boot contract (correct initial SP, correct `Reset_Handler`, initialised memory, a working interrupt) that the RTOS assumes on entry. Kept because the RTOS's claims start from a verified reset, not an assumed one. |
| 2 | [`rtos_kernel/`](rtos_kernel/) | The BTP proper: preemptive fixed-priority RTOS with poll-mode scheduling, cache-partitioned lock-free IPC, and a benchmarking testbed | The **project**. See [`rtos_kernel/docs/BTP_Objective.pdf`](rtos_kernel/docs/BTP_Objective.pdf) for objectives, method and results. |
| 3 | [`freertos_baseline/`](freertos_baseline/) | FreeRTOS V11.1.0 running the same experiments, on the same boot layer, with the same measurement code | The **control**. The objective is stated against FreeRTOS, so FreeRTOS has to be measured rather than cited. Head-to-head figures: [`COMPARISON_FREERTOS.md`](rtos_kernel/docs/COMPARISON_FREERTOS.md). |

Part 2 carries its own `startup/` and `ld/` so it builds standalone; they are
the Part 1 files extended with the `SVC`/`PendSV` vectors the kernel needs.

## Quick start

Both parts build with the GNU Arm Embedded toolchain and run under QEMU:

```bash
brew install --cask gcc-arm-embedded && brew install qemu   # macOS
# sudo apt install gcc-arm-none-eabi qemu-system-arm        # Ubuntu
```

**Part 1, the boot sequence**

```bash
cd baremetal_boot && make && make qemu
```

Prints the boot banner and a SysTick interrupt heartbeat. Exit with `Ctrl-A` then `X`.

**Part 2, the RTOS**

```bash
cd rtos_kernel && make && make qemu
```

**Part 3, the FreeRTOS baseline**

```bash
cd freertos_baseline && make bench-jitter && make qemu
```

Prints the schedulability analysis, the priority-inversion result, and periodic
latency reports. The three benchmark experiments are separate images, 
`make bench-jitter`, `make bench-ipc`, `make bench-ctxsw`; see that directory's
README for how to run them and why QEMU results need `-icount`.

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
