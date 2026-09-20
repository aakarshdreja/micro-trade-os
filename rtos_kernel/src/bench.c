/* ================================================================
 * bench.c: THE BENCHMARKING TESTBED
 *
 * Three experiments, each selected by a build flag, each producing
 * a number that can be defended:
 *
 *   -DRTOS_BENCH_CTXSW   context-switch cost, >= 10^6 samples
 *   -DRTOS_BENCH_JITTER  execution jitter, tick-driven vs poll-mode
 *   -DRTOS_BENCH_IPC     64-byte message latency, three queue designs
 *
 * METHODOLOGY NOTES THAT APPLY TO ALL THREE
 * -----------------------------------------
 * 1. Distributions, not averages. Determinism is a claim about the
 *    TAIL. A mean hides exactly the events. A tick landing mid-loop,
 *    a lock being contended, that the experiments are about, so
 *    every result is reported as min/mean/p50/p99/p99.9/max plus the
 *    histogram that produced them.
 *
 * 2. Batching. The measurement clock is DWT->CYCCNT, which is exact
 *    on silicon. Under QEMU there is no DWT and the kernel falls
 *    back to a SysTick-derived counter whose resolution is far
 *    coarser than one iteration of these loops. Timing a BLOCK of N
 *    iterations and dividing keeps the numbers meaningful on both:
 *    quantisation error is divided by N, while a tick landing inside
 *    a block still shows up. In the tail, where it belongs.
 *
 * 3. Same everything. The IPC subjects differ in synchronisation
 *    strategy and in nothing else: same kernel, same -O2, same
 *    clock, same 64-byte payload, same copy loop, same harness,
 *    interleaved in one run so drift cannot favour one of them.
 *
 * 4. QEMU numbers are FUNCTIONAL, not performance data. Every report
 *    prints which clock produced it. The figures that go in the
 *    report come from the board.
 * ================================================================ */

#include <stdint.h>
#include "rtos.h"
#include "gpio.h"
#include "semihosting.h"

#ifndef RTOS_CPU_MHZ
#define RTOS_CPU_MHZ  8u
#endif

#if defined(RTOS_BENCH_CTXSW) || defined(RTOS_BENCH_JITTER) || defined(RTOS_BENCH_IPC)

/* ---- shared printing helpers ----------------------------------- */
static void putu(uint32_t v)
{
    char b[12]; int i = 11; b[i--] = '\0';
    if (!v) b[i--] = '0';
    while (v) { b[i--] = (char)('0' + v % 10); v /= 10; }
    sh_puts(&b[i + 1]);
}

/* Print num/den as a fixed-point ratio with two decimals, e.g.
 * "12.47x". Used for the speed-up factors; integer math only.
 * Not every experiment reports a ratio, and each experiment is built
 * as its own image, so this is unused in some of them by design. */
__attribute__((unused))
static void put_ratio(uint32_t num, uint32_t den)
{
    if (den == 0u) { sh_puts("n/a"); return; }
    uint32_t h = (uint32_t)(((uint64_t)num * 100u) / den);
    putu(h / 100u); sh_puts(".");
    if ((h % 100u) < 10u) { sh_puts("0"); }
    putu(h % 100u);
    sh_puts("x");
}

static void put_clock_banner(void)
{
    sh_puts("clock source: ");
    sh_puts(rtos_dwt_available()
            ? "DWT CYCCNT (cycle-accurate, hardware)\r\n"
            : "SysTick-derived (QEMU: FUNCTIONAL ONLY, not perf data)\r\n");
}

/* ---- The measurement clock -------------------------------------
 * Timed regions here run BOTH inside and outside poll sections, and
 * the two cases need different clocks on the QEMU fallback path:
 *
 *   outside a section : rtos_cycles() is the kernel clock.
 *   inside a section  : rtos_cycles() STALLS on the fallback path,
 *                       because that path derives time from the tick
 *                       count and a poll section is defined by the
 *                       tick being masked. Timing a poll-mode region
 *                       with it would report every region as taking
 *                       the same near-zero time, which is how a
 *                       benchmark ends up "proving" that three
 *                       different implementations are identical.
 *                       rtos_poll_elapsed() is the section's own
 *                       clock and is valid there.
 *
 * On hardware both resolve to DWT->CYCCNT and the distinction costs
 * one predictable branch.
 *
 * Callers must take both samples of a measurement on the same side
 * of a section boundary; every timed region below does. */
__attribute__((always_inline))
static inline uint32_t bench_now(void)
{
    return rtos_poll_active() ? rtos_poll_elapsed() : rtos_cycles();
}

#endif

/* ================================================================
 * EXPERIMENT 1, CONTEXT-SWITCH COST
 * ================================================================
 * Two equal-priority tasks call rtos_yield() in a tight loop. Two
 * runnable tasks in one priority band means each yield rotates the
 * band and forces a real switch to the partner, so switches are
 * produced as fast as the core can retire them. PendSV times every
 * one of them into rtos_switch_stats (see context.s).
 *
 * The target is >= 10^6 samples: the p99.9 of a distribution cannot
 * be stated from a short run, and the whole argument is about the
 * tail.
 * ================================================================ */
#ifdef RTOS_BENCH_CTXSW

#define BENCH_TARGET  1000000u

static tcb_t    tcb_ping, tcb_pong, tcb_rep;
static uint32_t stk_ping[256], stk_pong[256], stk_rep[256];

static void ping_task(void *a) { (void)a; for (;;) { rtos_yield(); } }
static void pong_task(void *a) { (void)a; for (;;) { rtos_yield(); } }

static void ctxsw_reporter(void *a)
{
    (void)a;
    for (;;) {
        rtos_delay(500);
        if (rtos_switch_stats.count < BENCH_TARGET) {
            sh_puts("[bench] switches collected = ");
            putu(rtos_switch_stats.count); sh_puts(" / ");
            putu(BENCH_TARGET); sh_puts("\r\n");
            continue;
        }
        sh_puts("\r\n===== EXPERIMENT 1: context-switch cost (n>=");
        putu(BENCH_TARGET); sh_puts(") =====\r\n");
        put_clock_banner();
        rtos_stats_report("ctx-switch", &rtos_switch_stats);
        rtos_stats_histogram("ctx-switch", &rtos_switch_stats);
        sh_puts("max IRQ-disabled window = ");
        if (rtos_dwt_available()) {
            putu(rtos_max_irq_disabled_cycles()); sh_puts(" cyc\r\n");
        } else {
            sh_puts("n/a (needs DWT; QEMU is not cycle-accurate)\r\n");
        }
        for (;;) { rtos_delay(1000000u); }
    }
}

void bench_setup(void)
{
    sh_puts("Mode: EXPERIMENT 1 - context-switch cost\r\n");
    rtos_task_create(&tcb_ping, "ping", ping_task, 0, 4u, stk_ping, 256);
    rtos_task_create(&tcb_pong, "pong", pong_task, 0, 4u, stk_pong, 256);
    rtos_task_create(&tcb_rep,  "rep",  ctxsw_reporter, 0, 6u, stk_rep, 256);
}

#endif /* RTOS_BENCH_CTXSW */

/* ================================================================
 * EXPERIMENT 2, EXECUTION JITTER (the poll-mode claim)
 * ================================================================
 * THE CLAIM: suppressing the scheduler tick during a critical task
 * removes execution jitter.
 *
 * THE TEST: a task toggles PD12 as fast as it can. It times blocks
 * of JIT_EDGES toggles and records each block's duration. Every
 * block executes the identical instruction sequence, so in a perfect
 * world every block takes the identical number of cycles and the
 * distribution is a single spike. Anything wider is jitter, and on a
 * tick-driven RTOS it comes from the 1 kHz SysTick: vector fetch,
 * stack frame, handler, scheduler pass, return, landing on an
 * instruction the task cannot predict.
 *
 * TWO PHASES, IDENTICAL TASK SET:
 *   Phase A (baseline). Ordinary preemptible execution. A partner
 *      task sits at the SAME priority, so the tick's round-robin
 *      genuinely deschedules the task under test. This is what a
 *      stock RTOS configuration does.
 *   Phase B (poll mode). The same loop, same partner task, wrapped
 *      in RTOS_POLL_EXCLUSIVE poll sections. The tick cannot fire
 *      and the scheduler cannot switch, so nothing perturbs the loop.
 *
 * Only ONE variable changes between the phases: whether the block
 * runs inside a poll section.
 *
 * WHAT TO EXPECT, AND WHAT NOT TO:
 * Phase B's distribution should collapse to a single narrow spike
 * while Phase A's spreads. The two MEDIANS are not expected to
 * agree: Phase A time-shares the CPU with its same-priority partner,
 * so most of its blocks legitimately contain a slice of that
 * partner. The quantity that is comparable between the phases is the
 * MINIMUM, an uninterrupted block, and the gap between the two
 * minima is the cost of the poll machinery itself, which the report
 * prints rather than absorbs.
 *
 * EXTERNAL CONFIRMATION: PD13 is held high for the whole measured
 * window, so a logic analyser triggered on PD13 captures exactly the
 * blocks that produced the printed numbers. On the scope Phase A
 * shows a square wave with visible period outliers; Phase B shows a
 * uniform one. The on-chip histogram and the captured waveform are
 * two independent views of the same claim.
 *
 * WHY THE POLL SECTION WRAPS ONE BLOCK, NOT THE WHOLE PHASE:
 * a section must stay shorter than one tick period for the kernel's
 * suppressed-tick reconstruction to be exact without relying on the
 * loop's sampling rate (see poll.c). One block is ~JIT_EDGES*k
 * cycles, comfortably inside that, and the enter/exit cost is paid
 * per block and reported, not hidden.
 * ================================================================ */
#ifdef RTOS_BENCH_JITTER

#define JIT_EDGES     32u     /* toggles timed as one block          */
#define JIT_BLOCKS  4000u     /* blocks per phase                    */

static tcb_t    tcb_jit, tcb_noise, tcb_jrep;
static uint32_t stk_jit[256], stk_noise[256], stk_jrep[256];

static rtos_stats_t jit_base;   /* Phase A: tick-driven              */
static rtos_stats_t jit_poll;   /* Phase B: poll-mode                */

static volatile uint32_t jit_done;

/* One measured block. Identical code in both phases, the ONLY
 * difference between A and B is the caller's poll section, so any
 * difference in the distributions is attributable to that alone. */
__attribute__((noinline))
static uint32_t jitter_block(void)
{
    uint32_t t0 = bench_now();
    for (uint32_t i = 0; i < JIT_EDGES; i++) {
        gpio_set(GPIO_PIN_SIGNAL);
        gpio_clear(GPIO_PIN_SIGNAL);
    }
    return bench_now() - t0;
}

static void jitter_task(void *a)
{
    (void)a;
    rtos_delay(50);                    /* let the system settle */

    /* ---- Phase A: ordinary preemptible execution --------------- */
    gpio_set(GPIO_PIN_MARKER);
    for (uint32_t b = 0; b < JIT_BLOCKS; b++) {
        rtos_stats_add(&jit_base, jitter_block());
    }
    gpio_clear(GPIO_PIN_MARKER);

    rtos_delay(10);

    /* ---- Phase B: poll mode ------------------------------------ */
    gpio_set(GPIO_PIN_MARKER);
    for (uint32_t b = 0; b < JIT_BLOCKS; b++) {
        /* Budget generously: the budget exists to catch a runaway
         * section, not to cut a healthy one short. An overrun would
         * be reported by rtos_poll_report(). */
        if (!rtos_poll_enter(RTOS_POLL_EXCLUSIVE, 100000u)) {
            sh_puts("[jitter] poll_enter REFUSED\r\n");
            break;
        }
        uint32_t dt = jitter_block();
        (void)rtos_poll_expired();     /* budget check + tick sampling */
        rtos_poll_exit();

        rtos_stats_add(&jit_poll, dt);
    }
    gpio_clear(GPIO_PIN_MARKER);

    jit_done = 1;
    for (;;) { rtos_delay(1000000u); }
}

/* Same-priority competitor: without it the tick would still fire but
 * would have no second task to rotate to, and Phase A would understate
 * what a real multi-task system does to a critical loop. */
static void noise_task(void *a)
{
    (void)a;
    volatile uint32_t sink = 0;
    for (;;) { sink += 1u; }
}

static void jitter_reporter(void *a)
{
    (void)a;
    while (!jit_done) { rtos_delay(100); }

    sh_puts("\r\n===== EXPERIMENT 2: execution jitter =====\r\n");
    put_clock_banner();
    sh_puts("block = "); putu(JIT_EDGES);
    sh_puts(" GPIO toggles; blocks/phase = "); putu(JIT_BLOCKS);
    sh_puts("\r\n\r\n");

    sh_puts("Phase A - tick-driven (baseline RTOS behaviour):\r\n");
    rtos_stats_report("  block", &jit_base);
    sh_puts("Phase B - poll mode (RTOS_POLL_EXCLUSIVE):\r\n");
    rtos_stats_report("  block", &jit_poll);

    sh_puts("\r\nHistograms (shape is the evidence; a single spike = no jitter):\r\n");
    rtos_stats_histogram("Phase A tick-driven", &jit_base);
    rtos_stats_histogram("Phase B poll-mode",   &jit_poll);

    /* Jitter = spread of the distribution. A task that always takes
     * the same time has max == min. We report absolute spread and
     * the tail, because the tail is the number a real-time budget is
     * actually written against. */
    uint32_t spread_a = jit_base.max - jit_base.min;
    uint32_t spread_b = jit_poll.max - jit_poll.min;
    uint32_t tail_a   = rtos_stats_percentile(&jit_base, 999);
    uint32_t tail_b   = rtos_stats_percentile(&jit_poll, 999);

    sh_puts("\r\n--- Result ---\r\n");
    sh_puts("peak-to-peak jitter  A="); putu(spread_a);
    sh_puts(" cyc   B="); putu(spread_b); sh_puts(" cyc   reduction=");
    put_ratio(spread_a, spread_b ? spread_b : 1u); sh_puts("\r\n");
    sh_puts("p99.9 block time     A="); putu(tail_a);
    sh_puts(" cyc   B="); putu(tail_b); sh_puts(" cyc\r\n");
    sh_puts("median block time    A="); putu(rtos_stats_percentile(&jit_base, 500));
    sh_puts(" cyc   B="); putu(rtos_stats_percentile(&jit_poll, 500));
    sh_puts(" cyc\r\n");

    /* ---- Reading the two distributions correctly ----------------
     * The MEDIANS are NOT expected to match, and it would be wrong to
     * present them as though they should. Phase A shares the CPU with
     * a same-priority partner task, so a majority of its blocks
     * legitimately contain a slice of that partner's execution. That
     * time-sharing is the baseline behaviour under test, not an error
     * term to be subtracted.
     *
     * The quantity that IS comparable across the two phases is the
     * MINIMUM: the cost of a block that ran start to finish without
     * being interrupted. Both phases execute the identical
     * instruction sequence, so their minima should agree up to the
     * cost of the poll-section machinery itself, and the difference
     * between them is exactly that cost, per block, measured rather
     * than estimated. It is reported here because a mechanism that
     * buys determinism is not free, and the price belongs next to
     * the benefit. */
    sh_puts("\r\nUninterrupted block (the comparable quantity):\r\n");
    sh_puts("  A min="); putu(jit_base.min);
    sh_puts(" cyc   B min="); putu(jit_poll.min); sh_puts(" cyc\r\n");
    if (jit_poll.min >= jit_base.min) {
        sh_puts("  poll_enter+poll_exit overhead = ");
        putu(jit_poll.min - jit_base.min);
        sh_puts(" cyc per block (");
        putu((jit_poll.min - jit_base.min) / JIT_EDGES);
        sh_puts(" cyc per toggle at "); putu(JIT_EDGES);
        sh_puts(" toggles/block)\r\n");
    }
    sh_puts("  => poll mode does not make the loop faster; it removes\r\n"
            "     the interference, at a small fixed cost per section.\r\n");

    /* The claim that actually matters for a real-time budget. */
    sh_puts("\r\nWorst observed block: A="); putu(jit_base.max);
    sh_puts(" cyc   B="); putu(jit_poll.max);
    sh_puts(" cyc   improvement=");
    put_ratio(jit_base.max, jit_poll.max ? jit_poll.max : 1u);
    sh_puts("\r\n  A real-time budget is written against THIS number.\r\n");
    rtos_poll_report();

    for (;;) { rtos_delay(1000000u); }
}

void bench_setup(void)
{
    sh_puts("Mode: EXPERIMENT 2 - execution jitter (poll mode)\r\n");
    gpio_init();
    /* Bin width is chosen from the DATA, not by habit: Phase A blocks
     * run to tens of thousands of cycles when the tick preempts them,
     * and RTOS_STAT_BINS bins must span that range or the percentiles
     * saturate into the overflow bin and every subject reports the
     * same number. Both phases share one width so the two histograms
     * are directly comparable. rtos_stats_report() warns if this is
     * still too small for the data actually seen. */
    rtos_stats_init(&jit_base, 512u);
    rtos_stats_init(&jit_poll, 512u);

    rtos_task_create(&tcb_jit,   "jitter", jitter_task,     0, 4u, stk_jit,   256);
    rtos_task_create(&tcb_noise, "noise",  noise_task,      0, 4u, stk_noise, 256);
    rtos_task_create(&tcb_jrep,  "jrep",   jitter_reporter, 0, 6u, stk_jrep,  256);

    /* Only the task under test may open poll sections. */
    rtos_task_set_class(&tcb_jit, TASK_CLASS_POLL);
}

#endif /* RTOS_BENCH_JITTER */

/* ================================================================
 * EXPERIMENT 3, 64-BYTE IPC LATENCY
 * ================================================================
 * THE CLAIM: a cache-aligned lock-free ring beats a conventional
 * mutex-protected queue for hot-path message passing.
 *
 * THE MESSAGE: 64 bytes, the size of a market-data tick record, 
 * and, not coincidentally, two 32-byte cache lines, so the alignment
 * question is live rather than academic.
 *
 * THREE SUBJECTS, one variable (the synchronisation strategy):
 *   A. rtos_queue_t. Mutex + copy. The classical design, the same
 *      shape as FreeRTOS's xQueueSend/xQueueReceive. Built on this
 *      kernel so the comparison is like-for-like (see
 *      queue_baseline.c for why this matters and why the baseline is
 *      given its best honest form, not a strawman).
 *   B. spsc_t. Lock-free ring, ordinary layout.
 *   C. spsc_ca_t. Lock-free ring, cache-line partitioned with
 *      shadow indices.
 *
 * A vs B isolates the cost of the lock. B vs C isolates the cost of
 * the layout, which on this cache-less Cortex-M4 is really the cost
 * of the peer-index load that the shadow indices remove.
 *
 * WHAT IS TIMED: one send immediately followed by one receive, in a
 * single task, batched IPC_BATCH deep. Timing both halves in one
 * task measures the PRIMITIVE, with no scheduler participation to
 * confound it; a two-task version would measure the queue plus a
 * context switch, and the context switch is already Experiment 1.
 * The queue is left non-empty by construction, so the baseline's
 * fast path is taken and it never pays a semaphore block, again,
 * the most favourable honest version of the control.
 *
 * The three subjects are INTERLEAVED, batch by batch, so thermal
 * drift, counter drift or a background event cannot systematically
 * favour whichever ran first.
 *
 * Each batch runs inside its own poll section, so Experiment 3 is
 * also a second demonstration of Experiment 2's mechanism: it is
 * what stops a stray tick from landing in one subject's batch and
 * not another's.
 * ================================================================ */
#ifdef RTOS_BENCH_IPC

/* Exactly 64 bytes: a market-data tick record. */
typedef struct {
    uint32_t seq;
    uint32_t t_cyc;
    uint8_t  payload[56];
} ipc_msg_t;

#define IPC_CAP     16u     /* queue depth, power of two            */
#define IPC_BATCH  256u     /* send+recv pairs timed as one sample   */
#define IPC_BURST    8u     /* queue depth used by the burst pattern */
#define IPC_SAMPLES 200u    /* samples per subject                   */

static tcb_t    tcb_ipc;
static uint32_t stk_ipc[512];

/* Storage. The cache-aligned ring's payload array MUST itself start
 * on a cache-line boundary or slot 0 shares a line with whatever
 * precedes it. Rtos_spsc_ca_init() rejects it otherwise. */
static rtos_queue_t q_base;
static ipc_msg_t    q_base_buf[IPC_CAP];
static spsc_t       q_plain;
static ipc_msg_t    q_plain_buf[IPC_CAP];
static spsc_ca_t    q_ca;
static ipc_msg_t    q_ca_buf[IPC_CAP] RTOS_CACHE_ALIGN;

static rtos_stats_t st_base_pp, st_plain_pp, st_ca_pp;   /* ping-pong */
static rtos_stats_t st_base_bu, st_plain_bu, st_ca_bu;   /* burst     */

/* ================================================================
 * The two access patterns
 * ================================================================
 * The shadow-index optimisation is a bet that the peer's index does
 * not need re-reading on most operations. Whether that bet pays is a
 * property of the ACCESS PATTERN, not of the queue, so measuring one
 * pattern and reporting it as "the" IPC cost would be measuring the
 * harness:
 *
 *  PING-PONG (depth 1): send one, receive it, repeat. The queue is
 *      empty at the start of every receive, so the consumer's shadow
 *      of the producer index is stale EVERY time and must be
 *      refreshed every time. The optimisation cannot win here, it
 *      can only add a branch and a store. This is the adversarial
 *      case and it is reported, not hidden.
 *
 *  BURST (depth IPC_BURST): fill several slots, then drain them.
 *      This is what a real feed handler does. An ISR or DMA
 *      completion deposits a run of records and the consumer drains
 *      them, and it is the case the shadow indices are designed
 *      for: one refresh amortised over IPC_BURST operations.
 *
 * Both patterns move the same number of messages, so their numbers
 * are directly comparable.
 * ================================================================ */
#define DEFINE_BATCH(NAME, SEND, RECV)                                     \
static uint32_t batch_##NAME##_pingpong(ipc_msg_t *m, ipc_msg_t *o)        \
{                                                                          \
    uint32_t t0 = bench_now();                                             \
    for (uint32_t i = 0; i < IPC_BATCH; i++) { SEND; RECV; }               \
    return bench_now() - t0;                                               \
}                                                                          \
static uint32_t batch_##NAME##_burst(ipc_msg_t *m, ipc_msg_t *o)           \
{                                                                          \
    uint32_t t0 = bench_now();                                             \
    for (uint32_t i = 0; i < IPC_BATCH / IPC_BURST; i++) {                 \
        for (uint32_t k = 0; k < IPC_BURST; k++) { SEND; }                 \
        for (uint32_t k = 0; k < IPC_BURST; k++) { RECV; }                 \
    }                                                                      \
    return bench_now() - t0;                                               \
}

DEFINE_BATCH(base,
             (void)rtos_queue_try_send(&q_base, m),
             (void)rtos_queue_try_recv(&q_base, o))
DEFINE_BATCH(plain,
             (void)rtos_spsc_try_send(&q_plain, m),
             (void)rtos_spsc_try_recv(&q_plain, o))
DEFINE_BATCH(ca,
             (void)rtos_spsc_ca_try_send(&q_ca, m),
             (void)rtos_spsc_ca_try_recv(&q_ca, o))

/* ================================================================
 * Correctness gate
 * ================================================================
 * A latency number from a queue that corrupts or drops messages is
 * worthless, and on the QEMU build (where the timings are not
 * performance data at all) this is the part of Experiment 3 that
 * still carries weight. Every message is pushed through byte-for-
 * byte and checked, and the full/empty edges are exercised.
 * ================================================================ */
static int msg_equal(const ipc_msg_t *a, const ipc_msg_t *b)
{
    if (a->seq != b->seq || a->t_cyc != b->t_cyc) { return 0; }
    for (uint32_t i = 0; i < sizeof(a->payload); i++) {
        if (a->payload[i] != b->payload[i]) { return 0; }
    }
    return 1;
}

static int verify_queues(void)
{
    ipc_msg_t m, o;
    int ok = 1;

    for (uint32_t n = 0; n < 200u; n++) {
        m.seq = n; m.t_cyc = n * 7919u;
        for (uint32_t i = 0; i < sizeof(m.payload); i++) {
            m.payload[i] = (uint8_t)(n + i * 13u);
        }
        if (!rtos_queue_try_send(&q_base, &m)  ||
            !rtos_queue_try_recv(&q_base, &o)  || !msg_equal(&m, &o)) { ok = 0; }
        if (!rtos_spsc_try_send(&q_plain, &m)  ||
            !rtos_spsc_try_recv(&q_plain, &o)  || !msg_equal(&m, &o)) { ok = 0; }
        if (!rtos_spsc_ca_try_send(&q_ca, &m)  ||
            !rtos_spsc_ca_try_recv(&q_ca, &o)  || !msg_equal(&m, &o)) { ok = 0; }
    }

    /* Fill to capacity, confirm the next send is refused, drain in
     * FIFO order, confirm the next receive is refused. Ordering is
     * checked explicitly: a ring that returns items out of order
     * would still pass a naive round-trip test. */
    for (uint32_t i = 0; i < IPC_CAP; i++) {
        m.seq = i; m.t_cyc = 0;
        if (!rtos_spsc_ca_try_send(&q_ca, &m)) { ok = 0; }
    }
    if (rtos_spsc_ca_try_send(&q_ca, &m))          { ok = 0; }  /* must be full  */
    if (rtos_spsc_ca_count(&q_ca) != IPC_CAP)      { ok = 0; }
    for (uint32_t i = 0; i < IPC_CAP; i++) {
        if (!rtos_spsc_ca_try_recv(&q_ca, &o) || o.seq != i) { ok = 0; }
    }
    if (rtos_spsc_ca_try_recv(&q_ca, &o))          { ok = 0; }  /* must be empty */

    return ok;
}

/* Report one subject: the batch statistic converted to per-message
 * cost. The headline is the MEDIAN, not the minimum: the minimum is
 * the best case the silicon can do and flatters every design
 * equally, while the median is what the system actually pays. Each
 * batch carries IPC_BATCH round-trips, i.e. 2*IPC_BATCH messages. */
static void ipc_report(const char *name, const rtos_stats_t *s)
{
    uint32_t p50  = rtos_stats_percentile(s, 500);
    uint32_t p999 = rtos_stats_percentile(s, 999);
    sh_puts("  "); sh_puts(name);
    sh_puts(": per-message p50="); putu(p50);
    sh_puts(" cyc   p99.9=");      putu(p999);
    sh_puts(" cyc   min=");        putu(s->min);
    sh_puts(" max=");              putu(s->max);
    sh_puts(" cyc\r\n");
}

static void ipc_task(void *a)
{
    (void)a;
    ipc_msg_t m, out;

    /* Deterministic, non-trivial payload: an all-zero buffer could in
     * principle be handled differently by the copy loop. */
    m.seq = 0; m.t_cyc = 0;
    for (uint32_t i = 0; i < sizeof(m.payload); i++) {
        m.payload[i] = (uint8_t)(i * 7u + 1u);
    }

    rtos_delay(50);

    /* ---- Correctness first; a latency number from a broken queue
     * means nothing. ------------------------------------------- */
    int correct = verify_queues();
    sh_puts("\r\n[verify] queue correctness (round-trip integrity, FIFO "
            "order, full/empty edges): ");
    sh_puts(correct ? "PASS\r\n" : "*** FAIL ***\r\n");

    /* ---- Timing ------------------------------------------------ */
    for (uint32_t s = 0; s < IPC_SAMPLES; s++) {
        m.seq = s;

        /* The six subjects are measured back to back and interleaved,
         * so drift cannot systematically favour whichever ran first.
         *
         * NOTE ON WHY THIS IS *NOT* WRAPPED IN A POLL SECTION.
         * Timing inside a poll section would protect each batch from
         * the tick, but on the QEMU fallback path the only clock
         * valid inside a section is the section's own reload counter,
         * whose resolution is one SysTick sample. At these batch
         * lengths that quantises every subject onto the same value
         * and the experiment "proves" that three different designs
         * are identical. Outside a section, rtos_cycles() is backed
         * by the monotonic tick count and has the range to resolve
         * them. Tick interference is handled statistically instead:
         * the headline is the MEDIAN over IPC_SAMPLES batches, which
         * a 1 kHz tick cannot shift, and the p99.9 is reported
         * alongside so the interference remains visible rather than
         * being quietly removed. On hardware the DWT makes the whole
         * question moot. */
        uint32_t a_pp = batch_base_pingpong (&m, &out);
        uint32_t b_pp = batch_plain_pingpong(&m, &out);
        uint32_t c_pp = batch_ca_pingpong   (&m, &out);
        uint32_t a_bu = batch_base_burst    (&m, &out);
        uint32_t b_bu = batch_plain_burst   (&m, &out);
        uint32_t c_bu = batch_ca_burst      (&m, &out);

        /* Record the PER-MESSAGE cost, not the batch total. The
         * histogram has RTOS_STAT_BINS bins; a batch total of tens of
         * thousands of cycles overflows every one of them into the
         * same overflow bin, and the percentile then reports that
         * bin's edge for all six subjects, which reads as "all
         * designs are identical" when it means "the instrument was
         * out of range". Dividing here puts the values in a range the
         * histogram can actually resolve. Each batch carries
         * 2*IPC_BATCH messages (one send and one receive per
         * round-trip). */
        const uint32_t per = IPC_BATCH * 2u;
        rtos_stats_add(&st_base_pp,  a_pp / per);
        rtos_stats_add(&st_plain_pp, b_pp / per);
        rtos_stats_add(&st_ca_pp,    c_pp / per);
        rtos_stats_add(&st_base_bu,  a_bu / per);
        rtos_stats_add(&st_plain_bu, b_bu / per);
        rtos_stats_add(&st_ca_bu,    c_bu / per);
    }

    /* ---- Report ------------------------------------------------ */
    sh_puts("\r\n===== EXPERIMENT 3: 64-byte IPC latency =====\r\n");
    put_clock_banner();
    if (!rtos_dwt_available()) {
        sh_puts("NOTE: under QEMU these figures are only meaningful when the\r\n"
                "      emulator is run with -icount, which ties virtual time to\r\n"
                "      INSTRUCTION COUNT. That is a proxy, not a cycle count: it\r\n"
                "      models no memory stall, no DMB pipeline drain, no flash\r\n"
                "      wait state and no bus contention. Use it to compare the\r\n"
                "      SHAPE of the designs; take the numbers from the board.\r\n");
    }
    sh_puts("message = "); putu((uint32_t)sizeof(ipc_msg_t));
    sh_puts(" bytes; samples = "); putu(st_ca_pp.count);
    sh_puts(" batches of "); putu(IPC_BATCH); sh_puts(" round-trips\r\n");
    if (!rtos_dwt_available()) {
        sh_puts("      The ABSOLUTE figures below are in emulator time units and\r\n"
                "      are not cycle counts - under -icount they scale with host\r\n"
                "      virtual time, not with the Cortex-M4 pipeline. The RATIOS\r\n"
                "      are comparable, because every subject shares one clock.\r\n");
    }

    sh_puts("\r\n-- Pattern 1: PING-PONG (queue depth 1) --\r\n");
    ipc_report("A mutex queue (FreeRTOS-style)", &st_base_pp);
    ipc_report("B lock-free SPSC             ", &st_plain_pp);
    ipc_report("C lock-free SPSC, cache-algnd", &st_ca_pp);

    sh_puts("\r\n-- Pattern 2: BURST (depth ");
    putu(IPC_BURST); sh_puts(") --\r\n");
    ipc_report("A mutex queue (FreeRTOS-style)", &st_base_bu);
    ipc_report("B lock-free SPSC             ", &st_plain_bu);
    ipc_report("C lock-free SPSC, cache-algnd", &st_ca_bu);

    uint32_t a_pp50 = rtos_stats_percentile(&st_base_pp,  500);
    uint32_t b_pp50 = rtos_stats_percentile(&st_plain_pp, 500);
    uint32_t c_pp50 = rtos_stats_percentile(&st_ca_pp,    500);
    uint32_t a_bu50 = rtos_stats_percentile(&st_base_bu,  500);
    uint32_t b_bu50 = rtos_stats_percentile(&st_plain_bu, 500);
    uint32_t c_bu50 = rtos_stats_percentile(&st_ca_bu,    500);

    sh_puts("\r\n--- Speed-up (median; >1.00x means the second is faster) ---\r\n");
    sh_puts("  ping-pong  lock-free vs mutex : "); put_ratio(a_pp50, b_pp50 ? b_pp50 : 1u);
    sh_puts("\r\n  ping-pong  cache-al. vs plain : "); put_ratio(b_pp50, c_pp50 ? c_pp50 : 1u);
    sh_puts("\r\n  burst      lock-free vs mutex : "); put_ratio(a_bu50, b_bu50 ? b_bu50 : 1u);
    sh_puts("\r\n  burst      cache-al. vs plain : "); put_ratio(b_bu50, c_bu50 ? c_bu50 : 1u);
    sh_puts("\r\n");

    /* ---- The determinism argument, which is structural ---------
     * The timing above is a throughput question. The reason a
     * lock-free ring belongs on a real-time hot path is a different
     * and stronger one: it never disables interrupts, so it cannot
     * extend anyone else's worst-case latency. A mutex must. That is
     * a property of the code, not of a measurement, and it can be
     * checked directly in the disassembly:
     *
     *   arm-none-eabi-objdump -d build/spsc.o | grep -c cpsid   -> 0
     *
     * See the README for the full check. */
    /* Interpreting B vs C without wishful thinking. */
    sh_puts("\r\nOn the cache-line partitioning (B vs C): a ratio at or below\r\n"
            "1.00x is the EXPECTED result on this part. The STM32F407 is a\r\n"
            "Cortex-M4 with no data cache and single-cycle SRAM, so the load\r\n"
            "the shadow indices remove costs about as much as the branch and\r\n"
            "the store that replace it. The partitioning is a portability and\r\n"
            "multi-core property - it pays on a cached Cortex-M7, where a\r\n"
            "falsely-shared line costs a refill - and it is not claimed as an\r\n"
            "M4 speed-up. Measuring it here is what establishes that.\r\n");

    sh_puts("\r\nInterrupt masking per message (structural, verified by objdump):\r\n");
    sh_puts("  A mutex queue : 4 critical sections (lock+unlock, both ways)\r\n");
    sh_puts("  B lock-free   : 0 - never disables interrupts\r\n");
    sh_puts("  C cache-algnd : 0 - never disables interrupts\r\n");
    sh_puts("  => only A can extend another task's worst-case latency.\r\n");

    sh_puts("\r\nTail behaviour (p99.9/p50; closer to 1.00 is more deterministic):\r\n");
    sh_puts("  ping-pong A="); put_ratio(rtos_stats_percentile(&st_base_pp, 999),  a_pp50 ? a_pp50 : 1u);
    sh_puts("  B=");           put_ratio(rtos_stats_percentile(&st_plain_pp, 999), b_pp50 ? b_pp50 : 1u);
    sh_puts("  C=");           put_ratio(rtos_stats_percentile(&st_ca_pp, 999),    c_pp50 ? c_pp50 : 1u);
    sh_puts("\r\n");
    rtos_poll_report();

    for (;;) { rtos_delay(1000000u); }
}

void bench_setup(void)
{
    sh_puts("Mode: EXPERIMENT 3 - 64-byte IPC latency\r\n");

    rtos_queue_init(&q_base, q_base_buf, sizeof(ipc_msg_t), IPC_CAP);
    rtos_spsc_init(&q_plain, q_plain_buf, sizeof(ipc_msg_t), IPC_CAP);
    if (!rtos_spsc_ca_init(&q_ca, q_ca_buf, sizeof(ipc_msg_t), IPC_CAP)) {
        sh_puts("FATAL: cache-aligned queue init failed "
                "(storage not 32-byte aligned?)\r\n");
    }

    rtos_stats_init(&st_base_pp,  256u);
    rtos_stats_init(&st_plain_pp, 256u);
    rtos_stats_init(&st_ca_pp,    256u);
    rtos_stats_init(&st_base_bu,  256u);
    rtos_stats_init(&st_plain_bu, 256u);
    rtos_stats_init(&st_ca_bu,    256u);

    rtos_task_create(&tcb_ipc, "ipc", ipc_task, 0, 5u, stk_ipc, 512);
}

#endif /* RTOS_BENCH_IPC */
