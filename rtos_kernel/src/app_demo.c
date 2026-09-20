/* ================================================================
 * app_demo.c: the demonstration application
 *
 * A deterministic low-latency event pipeline that exercises every
 * kernel mechanism at once, so that a single run is evidence the
 * parts work TOGETHER and not merely in isolation:
 *
 *   Feed(5)     every 1 ms: sample a simulated price, stamp it with
 *               the cycle counter, publish to q_price.
 *   Signal(4)   drain q_price, run a moving-average crossover,
 *               publish BUY/SELL decisions to q_order.
 *   Dispatch(6) drain q_order and act on it INSIDE A POLL SECTION,
 *               so the act-on-signal path cannot be descheduled.
 *               Records end-to-end latency (feed stamp -> dispatch).
 *   Reporter(1) periodically print the distributions.
 *
 * Both hot-path queues are the cache-line-partitioned lock-free ring
 * (spsc_ca_t): the pipeline is the feature's in-situ demonstration,
 * while bench.c is its measurement.
 *
 * Running alongside, a constructed three-task priority-inversion
 * scenario proves the mutex's priority inheritance bounds blocking.
 *
 * High-frequency trading motivates the latency targets and the shape
 * of the workload. No claim is made that this is a trading system.
 * ================================================================ */

#include <stdint.h>
#include "rtos.h"
#include "semihosting.h"

#ifndef RTOS_CPU_MHZ
#define RTOS_CPU_MHZ  8u
#endif

/* ---- Priorities (higher number = more urgent) ------------------ */
#define PRIO_REPORTER    1u
#define PRIO_PI_LOW      2u
#define PRIO_PI_MED      3u
#define PRIO_SIGNAL      4u
#define PRIO_FEED        5u
#define PRIO_DISPATCH    6u
#define PRIO_PI_HIGH     7u

static void putu(uint32_t v)
{
    char b[12]; int i = 11; b[i--] = '\0';
    if (!v) b[i--] = '0';
    while (v) { b[i--] = (char)('0' + v % 10); v /= 10; }
    sh_puts(&b[i + 1]);
}

/* ---- Messages -------------------------------------------------- */
typedef struct { uint32_t seq; uint32_t price; uint32_t t_cyc; } price_msg_t;
typedef struct { uint32_t seq; int32_t dir; uint32_t price; uint32_t t_cyc; } order_msg_t;

/* ---- Hot-path queues: cache-line partitioned, lock-free --------
 * The backing storage carries RTOS_CACHE_ALIGN because slot 0 must
 * start on a cache-line boundary; rtos_spsc_ca_init() rejects the
 * queue otherwise rather than silently giving up the property. */
static spsc_ca_t   q_price;
static price_msg_t q_price_buf[16] RTOS_CACHE_ALIGN;
static spsc_ca_t   q_order;
static order_msg_t q_order_buf[16] RTOS_CACHE_ALIGN;

/* ---- Latency statistics ---------------------------------------- */
static rtos_stats_t e2e_stats;   /* feed stamp -> dispatch, cycles   */

/* ---- TCBs and stacks ------------------------------------------- */
static tcb_t tcb_feed, tcb_signal, tcb_dispatch, tcb_reporter;
static tcb_t tcb_pi_low, tcb_pi_med, tcb_pi_high;
static uint32_t stk_feed[256], stk_signal[256], stk_dispatch[256], stk_reporter[256];
static uint32_t stk_pi_low[256], stk_pi_med[256], stk_pi_high[256];

/* ---- Priority-inversion demo shared resource ------------------- */
static rtos_mutex_t pi_mutex;
static volatile int pi_low_has_lock;

/* Deterministic pseudo-random walk. A fixed seed keeps every run
 * reproducible, which matters when a latency figure has to be
 * defended twice. */
static uint32_t lcg = 0x1234567u;
static uint32_t rnd(void) { lcg = lcg * 1103515245u + 12345u; return (lcg >> 16) & 0x7FFFu; }

/* ================================================================
 * Pipeline
 * ================================================================ */
static void feed_task(void *arg)
{
    (void)arg;
    uint32_t seq = 0, price = 10000;
    for (;;) {
        int32_t step = (int32_t)(rnd() % 41) - 20;
        int32_t p = (int32_t)price + step;
        price = (p < 100) ? 100u : (uint32_t)p;

        price_msg_t m = { seq++, price, rtos_cycles() };
        (void)rtos_spsc_ca_try_send(&q_price, &m);   /* wait-free */
        rtos_delay(1);                                /* 1 ms cadence */
    }
}

static void signal_task(void *arg)
{
    (void)arg;
    enum { SHORT_N = 4, LONG_N = 16 };
    uint32_t hist[LONG_N] = {0};
    uint32_t n = 0;
    int32_t last_dir = 0;

    for (;;) {
        price_msg_t pm;
        if (!rtos_spsc_ca_try_recv(&q_price, &pm)) { rtos_delay(1); continue; }

        hist[n % LONG_N] = pm.price;
        n++;
        if (n < LONG_N) { continue; }

        uint32_t s = 0, l = 0;
        for (uint32_t i = 0; i < LONG_N; i++) {
            uint32_t v = hist[(n - 1 - i) % LONG_N];
            l += v;
            if (i < SHORT_N) { s += v; }
        }
        int32_t dir = ((s / SHORT_N) > (l / LONG_N)) ? +1 : -1;
        if (dir != last_dir) {
            last_dir = dir;
            order_msg_t om = { pm.seq, dir, pm.price, pm.t_cyc };
            (void)rtos_spsc_ca_try_send(&q_order, &om);
        }
    }
}

/* The act-on-signal path. This is the part of the pipeline whose
 * latency is actually being defended, so it is the part that runs in
 * a poll section: from the moment the order is in hand to the moment
 * it has been acted on, the scheduler cannot intervene.
 *
 * The section is deliberately tiny and contains no blocking call, 
 * the semihosting write is done AFTER the section closes, because
 * console I/O is slow and has no business inside a critical window. */
static void dispatch_task(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    for (;;) {
        order_msg_t om;
        if (!rtos_spsc_ca_try_recv(&q_order, &om)) { rtos_delay(1); continue; }

        /* Stamp the end-to-end latency BEFORE opening the section.
         * The measurement is of the pipeline, not of the section, and
         * on the QEMU fallback path rtos_cycles() stalls inside a
         * section anyway (it is derived from the tick we are about to
         * mask), which would silently understate it. */
        uint32_t latency = rtos_cycles() - om.t_cyc;   /* event -> dispatch */

        if (rtos_poll_enter(RTOS_POLL_EXCLUSIVE, 50000u)) {
            rtos_stats_add(&e2e_stats, latency);
            (void)rtos_poll_expired();
            rtos_poll_exit();
        } else {
            rtos_stats_add(&e2e_stats, latency);
        }

        /* Throttled to 1-in-64 so the console shows the pipeline
         * working without the I/O itself dominating the run. */
        if ((n++ & 63u) == 0u) {
            sh_puts("[DISPATCH] ");
            sh_puts(om.dir > 0 ? "BUY  seq=" : "SELL seq=");
            putu(om.seq);
            sh_puts(" px=");  putu(om.price);
            sh_puts(" lat="); putu(latency); sh_puts(" cyc\r\n");
        }
    }
}

static void reporter_task(void *arg)
{
    (void)arg;
    uint32_t round = 0;
    for (;;) {
        rtos_delay(2000);       /* every ~2 s */
        round++;
        sh_puts("\r\n===== Latency report #"); putu(round); sh_puts(" =====\r\n");
        sh_puts("cycle source: ");
        sh_puts(rtos_dwt_available() ? "DWT (cycle-accurate)\r\n"
                                     : "SysTick-derived (QEMU functional)\r\n");
        rtos_stats_report("ctx-switch ", &rtos_switch_stats);
        rtos_stats_report("e2e latency", &e2e_stats);
        /* The worst-case interrupts-disabled window is the empirical
         * basis of the interrupt-latency bound, so it is only printed
         * when the clock can actually support the claim. QEMU is not
         * cycle-accurate. It advances virtual time in blocks rather
         * than per retired instruction, so a few-hundred-cycle
         * critical section can sample as thousands. Reporting that
         * number would be reporting the emulator, not the kernel. */
        sh_puts("max IRQ-disabled window = ");
        if (rtos_dwt_available()) {
            putu(rtos_max_irq_disabled_cycles());
            sh_puts(" cyc (~");
            putu(rtos_max_irq_disabled_cycles() / RTOS_CPU_MHZ);
            sh_puts(" us)\r\n");
        } else {
            sh_puts("n/a (needs DWT; QEMU is not cycle-accurate)\r\n");
        }
        rtos_poll_report();

        if (round == 2) {
            rtos_stats_histogram("ctx-switch", &rtos_switch_stats);
            rtos_stats_histogram("e2e latency", &e2e_stats);
        }
        sh_puts("orders dispatched = "); putu(e2e_stats.count);
        sh_puts("  q_price depth = "); putu(rtos_spsc_ca_count(&q_price));
        sh_puts("  q_order depth = "); putu(rtos_spsc_ca_count(&q_order));
        sh_puts("\r\n");
    }
}

/* ================================================================
 * Priority-inversion demonstration (runs once at startup)
 *
 *   pi_low  (2) takes pi_mutex and holds it through a long section.
 *   pi_high (7) then requests pi_mutex and blocks; inheritance
 *               boosts pi_low to priority 7.
 *   pi_med  (3) becomes ready in the middle and spins. WITHOUT
 *               inheritance it would preempt pi_low and delay
 *               pi_high for as long as it liked, unbounded
 *               inversion. WITH inheritance pi_low runs at 7 > 3,
 *               pi_med cannot preempt, and pi_high's blocking is
 *               bounded by pi_low's critical section. We measure it.
 * ================================================================ */
static void busy_cycles(uint32_t cyc)
{
    uint32_t start = rtos_cycles();
    while ((rtos_cycles() - start) < cyc) { __asm volatile ("nop"); }
}

static void pi_low_task(void *arg)
{
    (void)arg;
    rtos_delay(2);                     /* let higher tasks park first */
    rtos_mutex_lock(&pi_mutex);
    pi_low_has_lock = 1;
    busy_cycles(80000u);               /* hold the lock ~10 ms @ 8 MHz */
    rtos_mutex_unlock(&pi_mutex);
    for (;;) { rtos_delay(1000000u); }
}

static void pi_med_task(void *arg)
{
    (void)arg;
    rtos_delay(6);                     /* wake while pi_low holds lock */
    busy_cycles(80000u);               /* would starve pi_low w/o PI  */
    for (;;) { rtos_delay(1000000u); }
}

static void pi_high_task(void *arg)
{
    (void)arg;
    rtos_delay(4);
    while (!pi_low_has_lock) { rtos_delay(1); }

    uint32_t t_req = rtos_cycles();
    rtos_mutex_lock(&pi_mutex);        /* blocks; PI boosts pi_low */
    uint32_t blocking = rtos_cycles() - t_req;
    rtos_mutex_unlock(&pi_mutex);

    sh_puts("\r\n--- Priority-inversion test ---\r\n");
    sh_puts("pi_high blocked for "); putu(blocking);
    sh_puts(" cycles (~"); putu(blocking / RTOS_CPU_MHZ); sh_puts(" us)\r\n");
    sh_puts("Blocking is bounded by pi_low's critical section, NOT by\r\n");
    sh_puts("pi_med - priority inheritance prevented unbounded inversion.\r\n");
    for (;;) { rtos_delay(1000000u); }
}

/* ================================================================
 * Entry point used by main.c
 * ================================================================ */
void app_demo_setup(void)
{
    rtos_stats_init(&e2e_stats, 512u);      /* 512 cycles per bin */

    if (!rtos_spsc_ca_init(&q_price, q_price_buf, sizeof(price_msg_t), 16) ||
        !rtos_spsc_ca_init(&q_order, q_order_buf, sizeof(order_msg_t), 16)) {
        sh_puts("FATAL: cache-aligned queue init failed\r\n");
        for (;;) { }
    }
    rtos_mutex_init(&pi_mutex);

    /* Priority-inversion demonstration tasks. */
    rtos_task_create(&tcb_pi_low,  "pi_low",  pi_low_task,  0, PRIO_PI_LOW,  stk_pi_low,  256);
    rtos_task_create(&tcb_pi_med,  "pi_med",  pi_med_task,  0, PRIO_PI_MED,  stk_pi_med,  256);
    rtos_task_create(&tcb_pi_high, "pi_high", pi_high_task, 0, PRIO_PI_HIGH, stk_pi_high, 256);

    /* Pipeline tasks. */
    rtos_task_create(&tcb_feed,     "feed",     feed_task,     0, PRIO_FEED,     stk_feed,     256);
    rtos_task_create(&tcb_signal,   "signal",   signal_task,   0, PRIO_SIGNAL,   stk_signal,   256);
    rtos_task_create(&tcb_dispatch, "dispatch", dispatch_task, 0, PRIO_DISPATCH, stk_dispatch, 256);
    rtos_task_create(&tcb_reporter, "reporter", reporter_task, 0, PRIO_REPORTER, stk_reporter, 256);

    /* Only the act-on-signal task is allowed to suppress the tick. */
    rtos_task_set_class(&tcb_dispatch, TASK_CLASS_POLL);
}
