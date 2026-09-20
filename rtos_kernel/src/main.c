/* ================================================================
 * main.c. System bring-up and build-mode selection
 *
 * Deliberately thin. Everything it does is common to every build:
 * start the cycle counter, print the schedulability analysis, create
 * the kernel, hand over to whichever payload this image was built
 * for, and start the scheduler.
 *
 * The payload is chosen at BUILD time, not run time, because the
 * benchmarks must not have the demo's tasks running alongside them, 
 * a measurement of the scheduler cannot share a CPU with unrelated
 * work and still be a measurement of the scheduler:
 *
 *   (default)            app_demo.c. The full pipeline demo
 *   -DRTOS_BENCH_CTXSW   bench.c, Experiment 1: switch cost
 *   -DRTOS_BENCH_JITTER  bench.c, Experiment 2: jitter
 *   -DRTOS_BENCH_IPC     bench.c, Experiment 3: IPC latency
 *
 * See the Makefile targets: make / make bench-ctxsw / bench-jitter /
 * bench-ipc.
 * ================================================================ */

#include <stdint.h>
#include "rtos.h"
#include "semihosting.h"

#if defined(RTOS_BENCH_CTXSW) || defined(RTOS_BENCH_JITTER) || defined(RTOS_BENCH_IPC)
#  define RTOS_BENCH_BUILD 1
void bench_setup(void);        /* bench.c    */
#else
void app_demo_setup(void);     /* app_demo.c */
#endif

int main(void)
{
    sh_puts("\r\n=====================================================\r\n");
    sh_puts("  Deterministic low-latency RTOS kernel (Cortex-M4)\r\n");
    sh_puts("  preemptive FP scheduler | PI mutex | poll-mode\r\n");
    sh_puts("  cache-aligned lock-free SPSC IPC\r\n");
    sh_puts("=====================================================\r\n");

    /* The cycle counter must come up before anything is timed. On
     * silicon this is DWT->CYCCNT; QEMU does not implement it, so the
     * kernel detects that and falls back to a SysTick-derived count.
     * Every report states which one produced its numbers. */
    rtos_cyccnt_enable();
    sh_puts("DWT cycle counter: ");
    sh_puts(rtos_dwt_available() ? "available (hardware, cycle-accurate)\r\n"
                                 : "unavailable -> SysTick fallback (QEMU)\r\n");

    /* Offline schedulability argument for the demo task set, printed
     * at boot so the measured latencies can be read against a bound
     * that was claimed BEFORE the run rather than after it. Times in
     * microseconds. Listed highest-priority first, as RTA requires. */
    static const rtos_taskspec_t specs[] = {
        { "dispatch", 1000, 50 },
        { "feed",     1000, 40 },
        { "signal",   1000, 70 },
    };
    rtos_schedulability_report(specs, 3);

    rtos_init();
    rtos_stats_init(&rtos_switch_stats, 8u);

#ifdef RTOS_BENCH_BUILD
    bench_setup();
#else
    app_demo_setup();
#endif

    sh_puts("Starting scheduler...\r\n");
    rtos_start();

    for (;;) { }
    return 0;
}
