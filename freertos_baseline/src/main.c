/* ================================================================
 * main.c: FreeRTOS BASELINE harness
 *
 * Runs the same experiments as rtos_kernel/src/bench.c, on
 * FreeRTOS instead of our kernel, so the two can be compared
 * directly.
 *
 * WHAT IS HELD IDENTICAL BETWEEN THE TWO IMAGES
 *   - the boot layer      (startup_stm32f4.s, linker.ld, byte-identical)
 *   - the measurement code(stats.c, the same histogram/percentile engine)
 *   - the cycle clock     (same SysTick-derived formula, below)
 *   - the GPIO driver     (gpio.c, same BSRR single-store toggle)
 *   - compiler and flags  (arm-none-eabi-gcc, -O2, soft float)
 *   - the emulated machine and the -icount setting
 *   - the workload itself (same block sizes, same message size,
 *                          same access patterns, same sample counts)
 *
 * WHAT DIFFERS
 *   - the kernel, which is the point.
 *
 * Only two of the three experiments are meaningful here. FreeRTOS
 * has no poll-mode equivalent, so Experiment 2 measures its
 * tick-driven behaviour, which is exactly the baseline our Phase A
 * models and our Phase B is claimed to improve on.
 * ================================================================ */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "semihosting.h"
#include "gpio.h"
#include "rtos.h"          /* for rtos_stats_t and the stats API only */

#define CPU_MHZ            8u
#define SYSTICK_RELOAD     ( CPU_MHZ * 1000u - 1u )
#define SYSTICK_VAL_REG    (*(volatile uint32_t *)0xE000E018UL)

/* ================================================================
 * Cycle clock, IDENTICAL formula to the RTOS build's fallback
 * (see rtos_cycles() in rtos_kernel/src/rtos.c).
 *
 * QEMU implements no DWT counter, so both kernels are timed the same
 * way: tick count times the period, plus the position within the
 * current tick. Using the same clock on both sides is what makes the
 * numbers comparable; using each kernel's "native" timing facility
 * would compare the facilities, not the kernels.
 * ================================================================ */
static uint32_t bench_cycles(void)
{
    uint32_t ticks, val;
    do {
        ticks = (uint32_t)xTaskGetTickCount();
        val   = SYSTICK_VAL_REG;
    } while (ticks != (uint32_t)xTaskGetTickCount());
    return ticks * (SYSTICK_RELOAD + 1u) + (SYSTICK_RELOAD - val);
}

void bench_assert_failed(const char *file, int line)
{
    sh_puts("\r\n*** configASSERT FAILED: ");
    sh_puts(file);
    sh_puts(":");
    { char b[12]; int i = 11; b[i--] = '\0'; int v = line;
      if (!v) b[i--] = '0';
      while (v) { b[i--] = (char)('0' + v % 10); v /= 10; }
      sh_puts(&b[i + 1]); }
    sh_puts(" ***\r\n");
    __asm volatile ("cpsid i");
    for (;;) { }
}

static void putu(uint32_t v)
{
    char b[12]; int i = 11; b[i--] = '\0';
    if (!v) b[i--] = '0';
    while (v) { b[i--] = (char)('0' + v % 10); v /= 10; }
    sh_puts(&b[i + 1]);
}

__attribute__((unused))
static void put_ratio(uint32_t num, uint32_t den)
{
    if (den == 0u) { sh_puts("n/a"); return; }
    uint32_t h = (uint32_t)(((uint64_t)num * 100u) / den);
    putu(h / 100u); sh_puts(".");
    if ((h % 100u) < 10u) { sh_puts("0"); }
    putu(h % 100u); sh_puts("x");
}

/* ================================================================
 * EXPERIMENT 2 (baseline half), execution jitter
 *
 * Identical to our Phase A: a task toggles PD12 and times blocks of
 * JIT_EDGES toggles, with a same-priority partner task running so
 * the tick's time-slicing genuinely deschedules it. FreeRTOS offers
 * no way to suppress its tick for a critical section, so there is no
 * Phase B on this side, that absence IS the finding.
 * ================================================================ */
#ifdef FRTOS_BENCH_JITTER
#define JIT_EDGES     32u
#define JIT_BLOCKS  4000u

static rtos_stats_t jit_frtos;
static volatile uint32_t jit_done;

__attribute__((noinline))
static uint32_t jitter_block(void)
{
    uint32_t t0 = bench_cycles();
    for (uint32_t i = 0; i < JIT_EDGES; i++) {
        gpio_set(GPIO_PIN_SIGNAL);
        gpio_clear(GPIO_PIN_SIGNAL);
    }
    return bench_cycles() - t0;
}

static void jitter_task(void *a)
{
    (void)a;
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set(GPIO_PIN_MARKER);
    for (uint32_t b = 0; b < JIT_BLOCKS; b++) {
        rtos_stats_add(&jit_frtos, jitter_block());
    }
    gpio_clear(GPIO_PIN_MARKER);
    jit_done = 1;
    for (;;) { vTaskDelay(portMAX_DELAY); }
}

static void noise_task(void *a)
{
    (void)a;
    volatile uint32_t sink = 0;
    for (;;) { sink += 1u; }
}

#endif /* FRTOS_BENCH_JITTER */

/* ================================================================
 * EXPERIMENT 3 (baseline half), 64-byte IPC latency
 *
 * The same 64-byte message, the same two access patterns, the same
 * batch and sample counts, through FreeRTOS's own queue.
 *
 * xQueueSend/xQueueReceive with a zero block time is FreeRTOS's
 * non-blocking fast path. The fairest comparison against our
 * non-blocking try_send/try_recv. The queue is never allowed to
 * block, exactly as in our harness.
 * ================================================================ */
#ifdef FRTOS_BENCH_IPC
typedef struct { uint32_t seq; uint32_t t_cyc; uint8_t payload[56]; } ipc_msg_t;

#define IPC_CAP      16u
#define IPC_BATCH   256u
#define IPC_BURST     8u
#define IPC_SAMPLES 200u

static QueueHandle_t  q_frtos;
static rtos_stats_t   st_frtos_pp, st_frtos_bu;

static uint32_t batch_pingpong(ipc_msg_t *m, ipc_msg_t *o)
{
    uint32_t t0 = bench_cycles();
    for (uint32_t i = 0; i < IPC_BATCH; i++) {
        (void)xQueueSend(q_frtos, m, 0);
        (void)xQueueReceive(q_frtos, o, 0);
    }
    return bench_cycles() - t0;
}

static uint32_t batch_burst(ipc_msg_t *m, ipc_msg_t *o)
{
    uint32_t t0 = bench_cycles();
    for (uint32_t i = 0; i < IPC_BATCH / IPC_BURST; i++) {
        for (uint32_t k = 0; k < IPC_BURST; k++) { (void)xQueueSend(q_frtos, m, 0); }
        for (uint32_t k = 0; k < IPC_BURST; k++) { (void)xQueueReceive(q_frtos, o, 0); }
    }
    return bench_cycles() - t0;
}

/* Explicit compare rather than memcmp(), so this build links the same
 * libc_stubs.c as the RTOS build without additions. Identical to
 * msg_equal() in rtos_kernel/src/bench.c. */
static int msg_equal(const ipc_msg_t *a, const ipc_msg_t *b)
{
    if (a->seq != b->seq || a->t_cyc != b->t_cyc) { return 0; }
    for (uint32_t i = 0; i < sizeof(a->payload); i++) {
        if (a->payload[i] != b->payload[i]) { return 0; }
    }
    return 1;
}

static int verify_queue(void)
{
    ipc_msg_t m, o;
    int ok = 1;
    for (uint32_t n = 0; n < 200u; n++) {
        m.seq = n; m.t_cyc = n * 7919u;
        for (uint32_t i = 0; i < sizeof(m.payload); i++) {
            m.payload[i] = (uint8_t)(n + i * 13u);
        }
        if (xQueueSend(q_frtos, &m, 0) != pdTRUE)    { ok = 0; }
        if (xQueueReceive(q_frtos, &o, 0) != pdTRUE) { ok = 0; }
        if (!msg_equal(&m, &o))                      { ok = 0; }
    }
    /* FIFO order and full/empty edges, as on our side. */
    for (uint32_t i = 0; i < IPC_CAP; i++) {
        m.seq = i; m.t_cyc = 0;
        if (xQueueSend(q_frtos, &m, 0) != pdTRUE) { ok = 0; }
    }
    if (xQueueSend(q_frtos, &m, 0) == pdTRUE) { ok = 0; }   /* must be full */
    for (uint32_t i = 0; i < IPC_CAP; i++) {
        if (xQueueReceive(q_frtos, &o, 0) != pdTRUE || o.seq != i) { ok = 0; }
    }
    if (xQueueReceive(q_frtos, &o, 0) == pdTRUE) { ok = 0; } /* must be empty */
    return ok;
}

static void ipc_task(void *a)
{
    (void)a;
    ipc_msg_t m, out;
    m.seq = 0; m.t_cyc = 0;
    for (uint32_t i = 0; i < sizeof(m.payload); i++) {
        m.payload[i] = (uint8_t)(i * 7u + 1u);
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    int correct = verify_queue();
    sh_puts("\r\n[verify] FreeRTOS queue correctness: ");
    sh_puts(correct ? "PASS\r\n" : "*** FAIL ***\r\n");

    const uint32_t per = IPC_BATCH * 2u;
    for (uint32_t s = 0; s < IPC_SAMPLES; s++) {
        m.seq = s;
        rtos_stats_add(&st_frtos_pp, batch_pingpong(&m, &out) / per);
        rtos_stats_add(&st_frtos_bu, batch_burst(&m, &out)    / per);
    }

    sh_puts("\r\n===== FreeRTOS BASELINE: 64-byte IPC latency =====\r\n");
    sh_puts("message = "); putu((uint32_t)sizeof(ipc_msg_t));
    sh_puts(" bytes; samples = "); putu(st_frtos_pp.count);
    sh_puts(" batches of "); putu(IPC_BATCH); sh_puts(" round-trips\r\n");
    sh_puts("  FreeRTOS xQueueSend/xQueueReceive, ping-pong: per-message p50=");
    putu(rtos_stats_percentile(&st_frtos_pp, 500)); sh_puts(" cyc\r\n");
    sh_puts("  FreeRTOS xQueueSend/xQueueReceive, burst    : per-message p50=");
    putu(rtos_stats_percentile(&st_frtos_bu, 500)); sh_puts(" cyc\r\n");
    rtos_stats_report("  ping-pong", &st_frtos_pp);
    rtos_stats_report("  burst    ", &st_frtos_bu);

    for (;;) { vTaskDelay(portMAX_DELAY); }
}

#endif /* FRTOS_BENCH_IPC */

/* ================================================================
 * EXPERIMENT 1 (baseline half), context-switch cost
 *
 * Measured through FreeRTOS's own trace hooks, which are the
 * supported mechanism for this and cost one call per switch, the
 * same shape of instrumentation our PendSV carries.
 * ================================================================ */
#ifdef FRTOS_BENCH_CTXSW
#define BENCH_TARGET 1000000u
static uint32_t sw_t0;
void bench_trace_switched_out(void) { sw_t0 = bench_cycles(); }
void bench_trace_switched_in(void)
{
    uint32_t dt = bench_cycles() - sw_t0;
    if (dt <= 20000u) { rtos_stats_record_switch(dt); }
}

static void ping_task(void *a) { (void)a; for (;;) { taskYIELD(); } }
static void ctx_reporter(void *a)
{
    (void)a;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (rtos_switch_stats.count < BENCH_TARGET) {
            sh_puts("[frtos bench] switches = ");
            putu(rtos_switch_stats.count); sh_puts(" / "); putu(BENCH_TARGET);
            sh_puts("\r\n");
            continue;
        }
        sh_puts("\r\n===== FreeRTOS BASELINE: context-switch cost =====\r\n");
        rtos_stats_report("ctx-switch", &rtos_switch_stats);
        rtos_stats_histogram("ctx-switch", &rtos_switch_stats);
        for (;;) { vTaskDelay(portMAX_DELAY); }
    }
}
#endif

/* ================================================================
 * Jitter reporter
 * ================================================================ */
#ifdef FRTOS_BENCH_JITTER
static void jitter_reporter(void *a)
{
    (void)a;
    while (!jit_done) { vTaskDelay(pdMS_TO_TICKS(100)); }
    sh_puts("\r\n===== FreeRTOS BASELINE: execution jitter =====\r\n");
    sh_puts("block = "); putu(JIT_EDGES);
    sh_puts(" GPIO toggles; blocks = "); putu(JIT_BLOCKS); sh_puts("\r\n");
    rtos_stats_report("  block", &jit_frtos);
    rtos_stats_histogram("  block", &jit_frtos);
    sh_puts("\r\npeak-to-peak jitter = "); putu(jit_frtos.max - jit_frtos.min);
    sh_puts(" cyc   worst block = "); putu(jit_frtos.max);
    sh_puts(" cyc   uninterrupted (min) = "); putu(jit_frtos.min);
    sh_puts(" cyc\r\n");
    sh_puts("FreeRTOS has no mechanism to suppress its scheduler tick for a\r\n"
            "critical section, so there is no second phase on this side.\r\n");
    for (;;) { vTaskDelay(portMAX_DELAY); }
}
#endif

/* ================================================================ */
int main(void)
{
    sh_puts("\r\n=====================================================\r\n");
    sh_puts("  FreeRTOS BASELINE  (kernel V11.1.0, ARM_CM3 port)\r\n");
    sh_puts("  8 MHz | 1 kHz tick | preemptive + time slicing\r\n");
    sh_puts("=====================================================\r\n");

    gpio_init();
    rtos_stats_init(&rtos_switch_stats, 8u);

#if defined(FRTOS_BENCH_JITTER)
    rtos_stats_init(&jit_frtos, 512u);
    xTaskCreate(jitter_task,     "jit",   256, NULL, 4, NULL);
    xTaskCreate(noise_task,      "noise", 128, NULL, 4, NULL);
    xTaskCreate(jitter_reporter, "jrep",  256, NULL, 6, NULL);
#elif defined(FRTOS_BENCH_IPC)
    rtos_stats_init(&st_frtos_pp, 256u);
    rtos_stats_init(&st_frtos_bu, 256u);
    q_frtos = xQueueCreate(IPC_CAP, sizeof(ipc_msg_t));
    if (q_frtos == NULL) { sh_puts("FATAL: xQueueCreate failed\r\n"); for (;;) {} }
    xTaskCreate(ipc_task, "ipc", 512, NULL, 5, NULL);
#elif defined(FRTOS_BENCH_CTXSW)
    xTaskCreate(ping_task,    "ping", 128, NULL, 4, NULL);
    xTaskCreate(ping_task,    "pong", 128, NULL, 4, NULL);
    xTaskCreate(ctx_reporter, "rep",  256, NULL, 6, NULL);
#else
    sh_puts("No experiment selected. Build with make bench-jitter|bench-ipc|bench-ctxsw\r\n");
#endif

    sh_puts("Starting FreeRTOS scheduler...\r\n");
    vTaskStartScheduler();
    for (;;) { }
    return 0;
}
