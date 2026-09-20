# Part 3: FreeRTOS Baseline

The control for the whole project. The objective is to be **better than
FreeRTOS**, which is a claim that can only be settled by running FreeRTOS
itself. On the same silicon model, the same toolchain, and the same workload.
This directory is that measurement.

## Why this exists separately from `rtos_kernel`

`rtos_kernel/src/queue_baseline.c` is a mutex-and-semaphore queue built the
way FreeRTOS builds one. It is a fair control for isolating *the cost of a
lock*, but it is **not FreeRTOS** and was never claimed to be. A project whose
objective is stated against FreeRTOS has to measure FreeRTOS.

As it turns out, measuring both was worth doing: the in-house baseline lands
within ~1% of the real kernel on the IPC benchmark, which is what justifies
treating it as a proxy in the first place. That agreement is a result, not an
assumption.

## What is held identical

The only intended variable is the kernel. Everything else is shared verbatim
with `rtos_kernel`:

| Held constant | How |
|---|---|
| Boot layer | `startup/startup_stm32f4.s` and `ld/linker.ld` copied byte-for-byte |
| Measurement code | the same `stats.c`, same histogram, same percentile engine |
| Cycle clock | same SysTick-derived formula (QEMU has no DWT on either side) |
| GPIO driver | the same `gpio.c`, same single-store BSRR toggle |
| Compiler and flags | `arm-none-eabi-gcc`, `-O2`, soft float, same warnings |
| Machine | `olimex-stm32-h405`, same `-icount shift=7` |
| Workload | same block sizes, message size, access patterns, sample counts |

FreeRTOS is configured to **match**, not to be handicapped: 8 MHz, 1 kHz tick,
preemptive with time-slicing, 8 priorities, `configUSE_PORT_OPTIMISED_TASK_SELECTION`
on (the CLZ-based selection our kernel also uses), priority-inheritance mutexes
on. Trace and runtime-stats facilities are off, because our kernel is not paying
for them either. Beating a crippled configuration would prove nothing.

## Layout

```
src/FreeRTOSConfig.h    configuration, with the reasoning for each choice
src/main.c              the three experiments, ported to FreeRTOS
src/{stats,gpio,libc_stubs}.c, semihosting.h, rtos.h
                        copied verbatim from rtos_kernel
startup/, ld/           copied verbatim from rtos_kernel
external/FreeRTOS-Kernel/
                        vendored FreeRTOS V11.1.0 (MIT), ARM_CM3 port
```

The **ARM_CM3** port is used deliberately: it targets Cortex-M without using the
FPU, matching our soft-float build. The ARM_CM4F port would add FPU context to
every switch that our kernel does not save, which would be a difference in the
work done rather than in the kernel design.

## Build and run

```bash
make bench-jitter    # Experiment 2 on FreeRTOS
make bench-ipc       # Experiment 3 on FreeRTOS
make bench-ctxsw     # Experiment 1 on FreeRTOS
```

Run with `-icount`, exactly as for our kernel. Without it QEMU's clock is too
coarse to separate the designs:

```bash
qemu-system-arm -machine olimex-stm32-h405 -nographic -icount shift=7 \
  -semihosting-config enable=on,target=native -kernel firmware.elf
```

Experiment 2 has only one phase here. FreeRTOS provides no way to suppress its
scheduler tick for a critical section, and that absence is precisely what the
comparison is about.

## Results

Head-to-head figures are in
[`../rtos_kernel/docs/COMPARISON_FREERTOS.md`](../rtos_kernel/docs/COMPARISON_FREERTOS.md).

## License

FreeRTOS is MIT-licensed; `external/FreeRTOS-Kernel/LICENSE.md` is included.
