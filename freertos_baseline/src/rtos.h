/* ================================================================
 * rtos.h: Public API for the deterministic, low-latency Cortex-M4
 * RTOS kernel.
 *
 * A minimal, preemptive, fixed-priority RTOS built on PendSV +
 * SysTick as the only hardware primitives, with worst-case latency
 * as a first-class, measured design constraint. Determinism, not
 * average throughput, is the goal. High-frequency trading is the
 * motivating domain for the latency targets and demo workload; no
 * claim is made that the platform is a trading system.
 *
 *   - Up to RTOS_MAX_PRIORITIES priority bands (higher = more urgent)
 *   - Round-robin scheduling within a band
 *   - O(1) highest-priority selection via CLZ on a ready bitmask
 *   - Binary/counting semaphores
 *   - Mutex with priority inheritance (bounds priority inversion)
 *   - Lock-free wait-free SPSC ring buffer for hot-path IPC
 *   - Cache-line-aligned SPSC variant with shadow indices (false-
 *     sharing elimination; see "Cache-aligned SPSC" below)
 *   - POLL-MODE scheduling: a task class that runs with the
 *     scheduler tick suppressed, for zero scheduling jitter
 *   - DWT cycle-accurate context-switch & interrupt-latency stats
 * ================================================================ */

#ifndef RTOS_H
#define RTOS_H

#include <stdint.h>
#include <stddef.h>

/* ---- Compile-time configuration -------------------------------- */
#define RTOS_MAX_PRIORITIES   8u    /* priority bands: 0 (idle) .. 7 */
#define RTOS_IDLE_PRIORITY    0u    /* reserved for the idle task     */

/* ---- Cache-line geometry ---------------------------------------
 * RTOS_CACHE_LINE is the granularity at which two variables stop
 * interfering with one another. On Cortex-M7/M55 (and on every
 * multi-core host) that granularity is the data-cache line: two hot
 * variables in the same line are invalidated together even though
 * the writers never touch the same address, "false sharing".
 *
 * The STM32F407 is a Cortex-M4: it has NO data cache, so on THIS
 * part the padding cannot buy a cache effect. What it does buy on
 * the M4 is (a) each index sits alone on its own naturally-aligned
 * 32-byte block, so a word access never straddles an SRAM bank
 * boundary, and (b) the data structure is already correct when the
 * same source is built for a cached Cortex-M7. The measured M4 win
 * comes from the SHADOW INDICES (see spsc_ca_t), which remove the
 * load of the peer's index from the hot path outright. This
 * distinction is stated plainly rather than papered over.
 * ---------------------------------------------------------------- */
#ifndef RTOS_CACHE_LINE
#define RTOS_CACHE_LINE       32u
#endif
#define RTOS_CACHE_ALIGN      __attribute__((aligned(RTOS_CACHE_LINE)))

/* ---- Cortex-M memory-ordering barriers ------------------------- */
static inline void rtos_dmb(void) { __asm volatile ("dmb 0xF" ::: "memory"); }
static inline void rtos_dsb(void) { __asm volatile ("dsb 0xF" ::: "memory"); }
static inline void rtos_isb(void) { __asm volatile ("isb 0xF" ::: "memory"); }

/* ---- Task states ----------------------------------------------- */
typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,     /* waiting on a semaphore/mutex                */
    TASK_DELAYED      /* sleeping for a number of ticks              */
} task_state_t;

/* ---- Task class ------------------------------------------------
 * TASK_CLASS_NORMAL. Ordinary preemptible task. The 1 kHz SysTick
 *                      may interrupt it at any instruction to run
 *                      the scheduler.
 * TASK_CLASS_POLL. A task permitted to open POLL SECTIONS via
 *                      rtos_poll_enter()/rtos_poll_exit(). Inside a
 *                      poll section the scheduler tick is suppressed
 *                      and the task runs to completion with zero
 *                      scheduling jitter. See poll.c.
 * ---------------------------------------------------------------- */
typedef enum {
    TASK_CLASS_NORMAL = 0,
    TASK_CLASS_POLL   = 1
} task_class_t;

/* ---- Task Control Block (TCB) ----------------------------------
 * 'sp' MUST be the first member: the PendSV assembly loads/stores
 * the saved stack pointer through the raw TCB pointer.
 * ---------------------------------------------------------------- */
typedef struct tcb {
    volatile uint32_t *sp;      /* saved process stack pointer (PSP) */
    struct tcb        *next;    /* intrusive list forward link       */
    struct tcb        *prev;    /* intrusive list backward link      */
    uint32_t           base_priority;  /* configured priority        */
    uint32_t           priority;       /* effective (may be inherited)*/
    volatile uint32_t  delay;   /* remaining ticks when DELAYED      */
    task_state_t       state;
    const char        *name;
    uint32_t          *stack_base;   /* lowest stack address (guard) */
    uint32_t           stack_words;  /* stack size in 32-bit words   */
    struct rtos_mutex *held_mutex;   /* mutex currently owned (or 0) */
    void              *wait_object;  /* object this task blocks on   */
    task_class_t       klass;        /* NORMAL or POLL (see poll.c)  */
} tcb_t;

typedef void (*task_entry_t)(void *arg);

/* ---- Kernel lifecycle ------------------------------------------ */
void rtos_init(void);
tcb_t *rtos_task_create(tcb_t *tcb, const char *name, task_entry_t entry,
                        void *arg, uint32_t priority,
                        uint32_t *stack, uint32_t stack_words);
void rtos_start(void);

/* ---- Task services --------------------------------------------- */
void     rtos_delay(uint32_t ticks);
void     rtos_yield(void);
uint32_t rtos_tick_count(void);
tcb_t   *rtos_current(void);

/* ================================================================
 * POLL-MODE SCHEDULING
 * ================================================================
 * A poll section is a bounded, non-blocking region of a
 * TASK_CLASS_POLL task during which the kernel guarantees that the
 * task is not descheduled. Inside it:
 *
 *   - the SysTick *interrupt* is masked (TICKINT cleared) while the
 *     SysTick *counter* keeps running, so the scheduler cannot
 *     preempt the task, yet no real time is lost;
 *   - rtos_schedule() becomes a no-op that records a DEFERRED
 *     switch instead of pending PendSV;
 *   - on exit the kernel replays the ticks that did not fire, so
 *     rtos_delay() deadlines and the tick count stay correct, and
 *     then takes the deferred switch.
 *
 * Two strengths:
 *
 *   RTOS_POLL_NO_TICK   Suppress the scheduler tick only. Device
 *                       interrupts (DMA/SPI/UART completion) are
 *                       still serviced, so I/O stays responsive.
 *                       This removes *scheduling* jitter.
 *
 *   RTOS_POLL_EXCLUSIVE Additionally raise BASEPRI to mask every
 *                       configurable-priority interrupt. Nothing
 *                       but a fault or NMI can run. This is the
 *                       true zero-jitter mode used for the GPIO
 *                       jitter benchmark.
 *
 * CONTRACT: a poll section must not block. rtos_delay(),
 * rtos_sem_take() and rtos_mutex_lock() have no one to switch to.
 * If a task breaks the contract the kernel force-closes the poll
 * section so the system stays live and counts the violation in
 * rtos_poll_info_t.violations rather than hanging silently.
 *
 * BUDGET: poll sections are cooperative by construction, so the
 * bound is the programmer's. rtos_poll_expired() lets the loop
 * honour a cycle budget, and doubles as the kernel's sampling hook
 * for counting suppressed SysTick reloads (see poll.c), call it
 * once per loop iteration.
 * ---------------------------------------------------------------- */
#define RTOS_POLL_NO_TICK     0u
#define RTOS_POLL_EXCLUSIVE   1u

void     rtos_task_set_class(tcb_t *t, task_class_t klass);

/* Open a poll section. 'budget_cycles' == 0 means "no budget".
 * Returns 1 on success, 0 if the caller is not TASK_CLASS_POLL or a
 * section is already open. */
int      rtos_poll_enter(uint32_t mode, uint32_t budget_cycles);

/* Close the poll section: replay suppressed ticks, restore the tick
 * interrupt and BASEPRI, then take any deferred context switch. */
void     rtos_poll_exit(void);

/* 1 while a poll section is open (any task). */
int      rtos_poll_active(void);

/* Budget check AND kernel timekeeping sample. Call once per
 * iteration of the poll loop. Returns 1 when the budget is spent. */
int      rtos_poll_expired(void);

/* Cycles elapsed inside the currently-open poll section. */
uint32_t rtos_poll_elapsed(void);

typedef struct {
    uint32_t sections;        /* poll sections opened               */
    uint32_t overruns;        /* sections that outran their budget  */
    uint32_t ticks_replayed;  /* SysTick ticks suppressed & replayed*/
    uint32_t deferred_switch; /* switches deferred to section exit  */
    uint32_t violations;      /* blocking calls inside a section    */
    uint32_t max_cycles;      /* longest section observed           */
} rtos_poll_info_t;

void     rtos_poll_info(rtos_poll_info_t *out);
void     rtos_poll_report(void);   /* prints the above              */

/* ---- Binary / counting semaphore ------------------------------- */
typedef struct rtos_sem {
    volatile int32_t count;
    int32_t          max_count;
    tcb_t           *waiters;
} rtos_sem_t;

void rtos_sem_init(rtos_sem_t *s, int32_t initial, int32_t max_count);
void rtos_sem_take(rtos_sem_t *s);
int  rtos_sem_try_take(rtos_sem_t *s);
void rtos_sem_give(rtos_sem_t *s);
void rtos_sem_give_from_isr(rtos_sem_t *s);

/* ---- Mutex with priority inheritance --------------------------- */
typedef struct rtos_mutex {
    tcb_t   *owner;
    tcb_t   *waiters;
    uint32_t owner_saved_prio;
} rtos_mutex_t;

void rtos_mutex_init(rtos_mutex_t *m);
void rtos_mutex_lock(rtos_mutex_t *m);
void rtos_mutex_unlock(rtos_mutex_t *m);

/* ---- Lock-free wait-free SPSC ring buffer (hot-path IPC) -------
 * Single producer, single consumer. No locks, no interrupt masking
 * on the hot path. Capacity must be a power of two. Correct memory
 * ordering between the data write and the index publish is enforced
 * with DMB barriers (see spsc.c).
 * ---------------------------------------------------------------- */
typedef struct spsc {
    uint8_t         *buffer;
    uint32_t         item_size;
    uint32_t         capacity;     /* power of two                   */
    uint32_t         mask;         /* capacity - 1                   */
    volatile uint32_t head;        /* consumer index (monotonic)     */
    volatile uint32_t tail;        /* producer index (monotonic)     */
} spsc_t;

void rtos_spsc_init(spsc_t *q, void *storage, uint32_t item_size,
                    uint32_t capacity_pow2);
/* Non-blocking, wait-free. Return 1 on success, 0 if full/empty.    */
int  rtos_spsc_try_send(spsc_t *q, const void *item);
int  rtos_spsc_try_recv(spsc_t *q, void *item);
uint32_t rtos_spsc_count(const spsc_t *q);

/* ================================================================
 * CACHE-ALIGNED LOCK-FREE SPSC RING BUFFER  (spsc_ca_t)
 * ================================================================
 * Same algorithm as spsc_t, two structural changes that matter on
 * the hot path:
 *
 * 1. CACHE-LINE PARTITIONING. The struct is split into three
 *    RTOS_CACHE_LINE-sized regions that never overlap:
 *
 *      line 0 : immutable geometry (buffer, item_size, mask), 
 *               read by both sides, written by neither after init
 *      line 1 : PRODUCER-OWNED  (tail, cached_head)
 *      line 2 : CONSUMER-OWNED  (head, cached_tail)
 *
 *    The producer never writes a line the consumer reads on its hot
 *    path and vice versa, so on a cached core (Cortex-M7, or any
 *    SMP host) the two sides cannot invalidate each other's line, 
 *    false sharing is eliminated by construction, not by luck.
 *
 * 2. SHADOW (CACHED) INDICES. The producer keeps 'cached_head', its
 *    own stale copy of the consumer's index, and consults only that
 *    copy to decide whether the ring is full. It re-reads the real
 *    'head' ONLY when the shadow says full. Which, on a queue that
 *    is kept drained, almost never happens. The consumer mirrors
 *    this with 'cached_tail'. The peer's line is therefore absent
 *    from the common-case path entirely.
 *
 *    This is the optimisation that pays on the cache-less Cortex-M4:
 *    it removes a load and a dependent compare from every send and
 *    every receive. (2) is what the benchmark measures; (1) is what
 *    makes the same source correct on a cached part.
 *
 * Ordering is unchanged: a DMB separates the payload write from the
 * index publish, exactly as in spsc_t.
 * ---------------------------------------------------------------- */
typedef struct spsc_ca {
    /* ---- line 0: immutable geometry, shared read-only ---------- */
    uint8_t  *buffer;
    uint32_t  item_size;
    uint32_t  capacity;          /* power of two                    */
    uint32_t  mask;              /* capacity - 1                    */
    uint8_t   _pad0[RTOS_CACHE_LINE - 4u * sizeof(uint32_t)];

    /* ---- line 1: producer-owned -------------------------------- */
    volatile uint32_t tail;      /* written by producer only        */
    uint32_t          cached_head; /* producer's shadow of head     */
    uint8_t   _pad1[RTOS_CACHE_LINE - 2u * sizeof(uint32_t)];

    /* ---- line 2: consumer-owned -------------------------------- */
    volatile uint32_t head;      /* written by consumer only        */
    uint32_t          cached_tail; /* consumer's shadow of tail     */
    uint8_t   _pad2[RTOS_CACHE_LINE - 2u * sizeof(uint32_t)];
} RTOS_CACHE_ALIGN spsc_ca_t;

/* 'storage' must itself be RTOS_CACHE_LINE aligned, declare it with
 * the RTOS_CACHE_ALIGN attribute. Returns 1 on success, 0 if the
 * capacity is not a power of two or the storage is misaligned. */
int  rtos_spsc_ca_init(spsc_ca_t *q, void *storage, uint32_t item_size,
                       uint32_t capacity_pow2);
int  rtos_spsc_ca_try_send(spsc_ca_t *q, const void *item);
int  rtos_spsc_ca_try_recv(spsc_ca_t *q, void *item);
uint32_t rtos_spsc_ca_count(const spsc_ca_t *q);

/* ================================================================
 * BASELINE QUEUE (queue_baseline.c). The control in the experiment
 * ================================================================
 * A deliberately conventional, mutex-plus-semaphore, copying queue
 * built the way a classical RTOS (FreeRTOS xQueueSend/xQueueReceive)
 * builds one: take a lock, memcpy the payload, release the lock,
 * signal a counting semaphore. It exists so the lock-free queues are
 * measured against a like-for-like implementation on the same
 * kernel, same compiler flags and same clock, instead of against a
 * number quoted from another project.
 * ---------------------------------------------------------------- */
typedef struct {
    uint8_t     *buffer;
    uint32_t     item_size;
    uint32_t     capacity;
    uint32_t     head;
    uint32_t     tail;
    uint32_t     count;
    rtos_mutex_t lock;
    rtos_sem_t   items;    /* counts filled slots  */
    rtos_sem_t   space;    /* counts free slots    */
} rtos_queue_t;

void rtos_queue_init(rtos_queue_t *q, void *storage, uint32_t item_size,
                     uint32_t capacity);
int  rtos_queue_try_send(rtos_queue_t *q, const void *item);
int  rtos_queue_try_recv(rtos_queue_t *q, void *item);
void rtos_queue_send(rtos_queue_t *q, const void *item);   /* blocking */
void rtos_queue_recv(rtos_queue_t *q, void *item);         /* blocking */

/* ---- Cycle counter (DWT on hardware; SysTick-derived fallback) - */
void     rtos_cyccnt_enable(void);
uint32_t rtos_cycles(void);        /* monotonic free-running cycles  */
int      rtos_dwt_available(void); /* 1 if the DWT counter is real   */

/* ---- Latency statistics (histogram + percentiles) -------------- */
#define RTOS_STAT_BINS   64u
typedef struct {
    uint32_t count;
    uint32_t min;
    uint32_t max;
    uint64_t sum;
    uint32_t bin_width;                 /* cycles per histogram bin  */
    uint32_t hist[RTOS_STAT_BINS + 1];  /* last bin = overflow       */
} rtos_stats_t;

void     rtos_stats_init(rtos_stats_t *s, uint32_t bin_width);
void     rtos_stats_add(rtos_stats_t *s, uint32_t sample);
uint32_t rtos_stats_percentile(const rtos_stats_t *s, uint32_t permille);
/* Samples that landed above the histogram's range. Non-zero means the
 * percentiles are approximate and bin_width was chosen too small. */
uint32_t rtos_stats_overflow(const rtos_stats_t *s);
void     rtos_stats_report(const char *label, const rtos_stats_t *s);
void     rtos_stats_histogram(const char *label, const rtos_stats_t *s);

/* Context-switch statistics maintained by PendSV. */
extern rtos_stats_t rtos_switch_stats;
void rtos_stats_record_switch(uint32_t cycles);  /* called from PendSV */

/* ---- Worst-case interrupt-latency instrumentation --------------
 * Tracks the longest interrupts-disabled (critical-section) window
 * observed at run time, in cycles. The empirical basis for the
 * analytic interrupt-latency bound.
 * ---------------------------------------------------------------- */
uint32_t rtos_max_irq_disabled_cycles(void);

/* ---- Schedulability analysis (RMA + response-time) ------------- */
typedef struct {
    const char *name;
    uint32_t    period;   /* T_i, ticks         */
    uint32_t    wcet;     /* C_i, ticks (or us) */
} rtos_taskspec_t;

/* Prints utilisation-bound check and response-time results, returns
 * 1 if the task set is provably schedulable, else 0. */
int rtos_schedulability_report(const rtos_taskspec_t *tasks, uint32_t n);

/* ---- MPU-based stack-overflow guard (hardware hardening) -------
 * rtos_mpu_init() turns the guard on once at startup (only compiled
 * in when built with -DRTOS_ENABLE_MPU, since QEMU's Cortex-M model
 * does not need it for functional verification). Thereafter the
 * kernel calls rtos_mpu_on_switch() from PendSV on every context
 * switch to place a small no-access MPU region at the bottom of the
 * task that is being switched IN, so an overflow of that task's
 * stack raises a MemManage fault instead of corrupting memory.
 * When the guard is not enabled both functions are cheap no-ops. */
void rtos_mpu_init(void);
void rtos_mpu_on_switch(void);   /* called from PendSV (context.s)   */

/* ---- Internal hooks used by assembly / SysTick ----------------- */
void rtos_tick_handler(void);
void rtos_schedule(void);
extern tcb_t *rtos_current_task;
extern tcb_t *rtos_next_task;

#endif /* RTOS_H */
