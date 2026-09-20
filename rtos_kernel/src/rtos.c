/* ================================================================
 * rtos.c: Core kernel: scheduler, task management, system tick
 *
 * Preemptive, fixed-priority, round-robin-within-band scheduler.
 * Highest-priority ready task is selected in O(1) using the CLZ
 * (count-leading-zeros) instruction on a ready bitmask, the same
 * trick used in production low-latency systems.
 * ================================================================ */

#include "rtos.h"
#include "rtos_internal.h"
#include "semihosting.h"

/* ================================================================
 * Cortex-M4 core peripheral registers (memory-mapped, no HAL)
 * ================================================================ */
#define SYSTICK_BASE   0xE000E010UL
#define SYSTICK_CTRL   (*(volatile uint32_t *)(SYSTICK_BASE + 0x00U))
#define SYSTICK_LOAD   (*(volatile uint32_t *)(SYSTICK_BASE + 0x04U))
#define SYSTICK_VAL    (*(volatile uint32_t *)(SYSTICK_BASE + 0x08U))
#define SYSTICK_CTRL_ENABLE    (1UL << 0)
#define SYSTICK_CTRL_TICKINT   (1UL << 1)
#define SYSTICK_CTRL_CLKSOURCE (1UL << 2)

/* System Control Block */
#define SCB_ICSR       (*(volatile uint32_t *)0xE000ED04UL)
#define SCB_ICSR_PENDSVSET  (1UL << 28)
#define SCB_SHPR3      (*(volatile uint32_t *)0xE000ED20UL) /* PendSV/SysTick prio */

/* Data Watchpoint & Trace unit. Cycle-accurate latency counter */
#define DWT_CTRL       (*(volatile uint32_t *)0xE0001000UL)
#define DWT_CYCCNT     (*(volatile uint32_t *)0xE0001004UL)
#define DWT_LAR        (*(volatile uint32_t *)0xE0001FB0UL) /* lock access  */
#define DWT_CTRL_CYCCNTENA  (1UL << 0)
#define CORE_DEMCR     (*(volatile uint32_t *)0xE000EDFCUL)
#define CORE_DEMCR_TRCENA   (1UL << 24)

/* Memory Protection Unit (for stack-overflow guard) */
#define MPU_TYPE       (*(volatile uint32_t *)0xE000ED90UL)
#define MPU_CTRL       (*(volatile uint32_t *)0xE000ED94UL)
#define MPU_RNR        (*(volatile uint32_t *)0xE000ED98UL)
#define MPU_RBAR       (*(volatile uint32_t *)0xE000ED9CUL)
#define MPU_RASR       (*(volatile uint32_t *)0xE000EDA0UL)
#define SCB_SHCSR      (*(volatile uint32_t *)0xE000ED24UL)
#define SCB_SHCSR_MEMFAULTENA (1UL << 16)

/* SysTick reload: QEMU olimex-stm32-h405 runs the core at ~8 MHz,
 * so RELOAD = 7999 gives a 1 ms (1 kHz) system tick. On real 168 MHz
 * silicon RTOS_CPU_MHZ becomes 168 and RELOAD 167999. */
#ifndef RTOS_CPU_MHZ
#define RTOS_CPU_MHZ         8UL       /* QEMU model clock (MHz)         */
#endif
#ifndef RTOS_SYSTICK_RELOAD
#define RTOS_SYSTICK_RELOAD  (RTOS_CPU_MHZ * 1000UL - 1UL)  /* 1 kHz tick */
#endif

/* ================================================================
 * Cycle counter with worst-case interrupt-latency instrumentation
 * ================================================================ */
static uint32_t   g_dwt_ok;                 /* 1 if DWT CYCCNT is real   */
static volatile uint32_t g_max_irq_off;     /* longest IRQ-off window    */

/* Forward decls for the cycle counter (defined below). */
uint32_t rtos_cycles(void);

/* ================================================================
 * Critical-section helpers (PRIMASK save/restore)
 *
 * We also measure the duration of every interrupts-disabled window
 * so the kernel can report the longest critical section observed, 
 * the empirical basis for the analytic worst-case interrupt-latency
 * bound this kernel reports.
 * ================================================================ */
static inline uint32_t enter_critical(void)
{
    uint32_t primask;
    __asm volatile ("mrs %0, primask" : "=r"(primask));
    __asm volatile ("cpsid i" ::: "memory");
    return primask;
}
static inline void exit_critical(uint32_t primask)
{
    __asm volatile ("msr primask, %0" :: "r"(primask) : "memory");
}

uint32_t rtos_max_irq_disabled_cycles(void) { return g_max_irq_off; }

/* ================================================================
 * Raw sub-tick clock. Used ONLY to time interrupts-disabled
 * windows.
 *
 * rtos_cycles() is the right clock for task-visible durations, but
 * it is the wrong one here: on the fallback path it is built from
 * g_ticks, and the window being measured is precisely the one in
 * which g_ticks is incremented, so a delta across it would include a
 * whole spurious tick.
 *
 * These two helpers read a counter with no tick-count term. On
 * silicon that is DWT->CYCCNT, which counts UP and is monotonic. On
 * QEMU it is SysTick's own VAL register, which counts DOWN and
 * wraps once per tick. Correct here because an interrupts-disabled
 * window is by construction far shorter than a tick, so at most one
 * reload can fall inside it.
 * ================================================================ */
static inline uint32_t raw_ticker(void)
{
    return g_dwt_ok ? DWT_CYCCNT : SYSTICK_VAL;
}

/* Cycles from sample 'a' to the later sample 'b'. Returns 1 and
 * writes *out when the window is measurable, 0 when the sample must
 * be discarded.
 *
 * On silicon (DWT) every sample is measurable: the counter is
 * monotonic and the subtraction is exact.
 *
 * On the QEMU fallback the clock is SysTick's VAL, which counts DOWN
 * and reloads once per tick, so the normal case is a >= b. When
 * b > a the sample is AMBIGUOUS: either the counter reloaded inside
 * the window, or QEMU, which is not cycle-accurate, and derives VAL
 * from virtual time rather than from retired instructions, simply
 * returned a slightly larger value than the previous read. Both look
 * identical from here, and assuming the former turns a ~100-cycle
 * window into a reported ~8000, which is how a 1 ms "worst-case
 * interrupts-disabled window" appears on a handler that plainly
 * cannot take 1 ms.
 *
 * So we discard the ambiguous samples instead of inventing a number
 * for them. That makes the QEMU statistic slightly conservative, it
 * cannot observe a window that happens to span a reload, and leaves
 * it honest, which matters more, because this figure is the basis of
 * the interrupt-latency bound. On hardware nothing is discarded and
 * the bound is measured exactly. */
static inline int raw_delta(uint32_t a, uint32_t b, uint32_t *out)
{
    if (g_dwt_ok) {
        *out = b - a;                  /* counts UP, monotonic       */
        return 1;
    }
    if (a >= b) {
        *out = a - b;                  /* counts DOWN, no reload     */
        return 1;
    }
    return 0;                          /* ambiguous: do not guess    */
}

/* ================================================================
 * Scheduler state
 * ================================================================ */
tcb_t *rtos_current_task = 0;   /* running task  (shared with context.s) */
tcb_t *rtos_next_task    = 0;   /* selected task (shared with context.s) */

static tcb_t   *ready_head[RTOS_MAX_PRIORITIES]; /* circular per-band list */
static uint32_t ready_mask;                       /* bit p set => band p ready */
static tcb_t   *delayed_head;                     /* circular list of sleepers */
static volatile uint32_t g_ticks;
static uint32_t started;

/* Idle task. Always runnable at the lowest priority. */
static tcb_t   idle_tcb;
static uint32_t idle_stack[64];

/* ================================================================
 * Intrusive circular doubly-linked list helpers
 * ================================================================ */
static void list_insert_tail(tcb_t **head, tcb_t *t)
{
    if (*head == 0) {
        t->next = t;
        t->prev = t;
        *head = t;
    } else {
        tcb_t *h = *head;
        t->next = h;
        t->prev = h->prev;
        h->prev->next = t;
        h->prev = t;
    }
}

static void list_remove(tcb_t **head, tcb_t *t)
{
    if (t->next == t) {
        *head = 0;
    } else {
        t->prev->next = t->next;
        t->next->prev = t->prev;
        if (*head == t) {
            *head = t->next;
        }
    }
    t->next = 0;
    t->prev = 0;
}

/* ================================================================
 * Ready-queue management
 * ================================================================ */
static void ready_add(tcb_t *t)
{
    t->state = TASK_READY;
    list_insert_tail(&ready_head[t->priority], t);
    ready_mask |= (1UL << t->priority);
}

static void ready_remove(tcb_t *t)
{
    uint32_t p = t->priority;
    list_remove(&ready_head[p], t);
    if (ready_head[p] == 0) {
        ready_mask &= ~(1UL << p);
    }
}

/* O(1) highest ready priority via CLZ. ready_mask must be non-zero
 * (guaranteed by the always-ready idle task). */
static inline uint32_t highest_ready_prio(void)
{
    return 31UL - (uint32_t)__builtin_clz(ready_mask);
}

/* ================================================================
 * rtos_schedule. Choose the next task and pend a context switch.
 * Safe to call from task, tick, or with interrupts already masked.
 * ================================================================ */
void rtos_schedule(void)
{
    /* POLL-MODE INTERLOCK. While a poll section is open the running
     * task owns the CPU by contract, so we must not pend a switch, 
     * not even on behalf of a higher-priority task readied by an
     * ISR. Record that a switch is owed and let rtos_poll_exit()
     * take it. This is what makes the zero-jitter guarantee a
     * property of the kernel rather than a side effect of the tick
     * happening to be masked. */
    if (rtos__poll_lock) {
        rtos__poll_deferred = 1;
        return;
    }

    uint32_t p = highest_ready_prio();
    tcb_t *candidate = ready_head[p];

    rtos_next_task = candidate;
    if (rtos_next_task != rtos_current_task) {
        SCB_ICSR = SCB_ICSR_PENDSVSET;   /* trigger PendSV_Handler */
    }
}

/* ================================================================
 * Task creation. Build an initial exception stack frame so the
 * first context switch "returns" into the task entry point.
 * ================================================================ */
tcb_t *rtos_task_create(tcb_t *tcb,
                        const char *name,
                        task_entry_t entry,
                        void *arg,
                        uint32_t priority,
                        uint32_t *stack,
                        uint32_t stack_words)
{
    if (priority >= RTOS_MAX_PRIORITIES) {
        priority = RTOS_MAX_PRIORITIES - 1;
    }

    /* Stack top, 8-byte aligned (AAPCS requirement). */
    uint32_t *sp = stack + stack_words;
    sp = (uint32_t *)(((uint32_t)sp) & ~0x7UL);

    /* Hardware-saved frame (restored automatically on exception return) */
    *(--sp) = 0x01000000UL;         /* xPSR: Thumb bit set              */
    *(--sp) = (uint32_t)entry;      /* PC : task entry point            */
    *(--sp) = 0xFFFFFFFDUL;         /* LR : should never return         */
    *(--sp) = 0;                    /* R12                              */
    *(--sp) = 0;                    /* R3                               */
    *(--sp) = 0;                    /* R2                               */
    *(--sp) = 0;                    /* R1                               */
    *(--sp) = (uint32_t)arg;        /* R0 : task argument               */

    /* Software-saved frame (restored by PendSV/SVC) */
    *(--sp) = 0;  /* R11 */
    *(--sp) = 0;  /* R10 */
    *(--sp) = 0;  /* R9  */
    *(--sp) = 0;  /* R8  */
    *(--sp) = 0;  /* R7  */
    *(--sp) = 0;  /* R6  */
    *(--sp) = 0;  /* R5  */
    *(--sp) = 0;  /* R4  */

    tcb->sp            = sp;
    tcb->name          = name;
    tcb->base_priority = priority;
    tcb->priority      = priority;
    tcb->delay         = 0;
    tcb->state         = TASK_READY;
    tcb->stack_base    = stack;
    tcb->stack_words   = stack_words;
    tcb->held_mutex    = 0;
    tcb->wait_object   = 0;
    tcb->klass         = TASK_CLASS_NORMAL;  /* opt in via rtos_task_set_class */

    /* Stack canary for overflow detection (checked in idle/HardFault). */
    stack[0] = 0xDEADBEEFUL;

    uint32_t pm = enter_critical();
    ready_add(tcb);
    exit_critical(pm);
    return tcb;
}

/* ================================================================
 * Idle task. Runs when nothing else is ready. Waits for interrupt
 * to save power and checks stack canaries.
 * ================================================================ */
static void idle_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        __asm volatile ("wfi");
    }
}

/* ================================================================
 * rtos_init / rtos_start
 * ================================================================ */
void rtos_init(void)
{
    ready_mask   = 0;
    delayed_head = 0;
    g_ticks      = 0;
    started      = 0;
    for (uint32_t i = 0; i < RTOS_MAX_PRIORITIES; i++) {
        ready_head[i] = 0;
    }
    rtos_current_task = 0;
    rtos_next_task    = 0;

    rtos_task_create(&idle_tcb, "idle", idle_task_fn, 0,
                     RTOS_IDLE_PRIORITY, idle_stack,
                     sizeof(idle_stack) / sizeof(idle_stack[0]));
}

void rtos_start(void)
{
    /* PendSV must be the LOWEST exception priority so a context
     * switch only happens after all other ISRs complete. SysTick
     * one notch above. SHPR3: [31:24]=SysTick, [23:16]=PendSV. */
    SCB_SHPR3 = (0xE0UL << 24) | (0xFFUL << 16);

    /* Enable the MPU stack-overflow guard (hardware builds only). */
    rtos_mpu_init();

    /* Select the first task to run WITHOUT pending PendSV. If we
     * pended a context switch here it would fire immediately (PendSV
     * is enabled, PRIMASK clear) while rtos_current_task is still 0
     * and PSP is uninitialised. Corrupting the first switch. The
     * first task is launched exclusively through SVC below. */
    rtos_current_task = 0;
    rtos_next_task    = ready_head[highest_ready_prio()];
    started           = 1;

    /* Configure and start the 1 kHz system tick. */
    SYSTICK_CTRL = 0;
    SYSTICK_LOAD = RTOS_SYSTICK_RELOAD;
    SYSTICK_VAL  = 0;
    SYSTICK_CTRL = SYSTICK_CTRL_CLKSOURCE |
                   SYSTICK_CTRL_TICKINT   |
                   SYSTICK_CTRL_ENABLE;


    /* Launch the first task via SVC (see SVC_Handler in context.s).
     * Interrupts MUST stay enabled: SVC is a synchronous exception
     * and would escalate to a HardFault if PRIMASK were set. */
    extern void rtos_start_first_task(void);
    rtos_start_first_task();

    /* Not reached. */
    for (;;) { }
}

/* ================================================================
 * Task services
 * ================================================================ */
uint32_t rtos_tick_count(void) { return g_ticks; }
tcb_t   *rtos_current(void)    { return rtos_current_task; }

void rtos_yield(void)
{
    uint32_t pm = enter_critical();
    /* Rotate this task to the back of its band, then reschedule. */
    uint32_t p = rtos_current_task->priority;
    if (ready_head[p] != 0) {
        ready_head[p] = ready_head[p]->next;
    }
    rtos_schedule();
    exit_critical(pm);
}

void rtos_delay(uint32_t ticks)
{
    if (ticks == 0) {
        rtos_yield();
        return;
    }
    uint32_t pm = enter_critical();
    /* Sleeping inside a poll section is the same contract violation
     * as blocking on a semaphore, see rtos__block_current_on(). */
    if (rtos__poll_lock) {
        rtos__poll_violation();
    }
    tcb_t *t = rtos_current_task;
    ready_remove(t);
    t->delay = ticks;
    t->state = TASK_DELAYED;
    list_insert_tail(&delayed_head, t);
    rtos_schedule();
    exit_critical(pm);
}

/* ================================================================
 * rtos_tick_handler. Invoked from SysTick_Handler (1 kHz).
 * Wakes expired sleepers and applies round-robin within the band.
 * ================================================================ */
/* ================================================================
 * rtos__tick_advance. Move the kernel time base forward by n ticks
 * in a single pass over the sleeper list.
 *
 * Two callers: the 1 kHz tick handler (n == 1) and rtos_poll_exit(),
 * which replays the ticks suppressed during a poll section (n may be
 * large). Handling n > 1 in one pass keeps the replay O(sleepers)
 * rather than O(sleepers * n), so closing a long poll section costs
 * the same as closing a short one. A property poll mode needs, since
 * the replay itself runs with interrupts masked.
 *
 * The list is circular and is mutated as we walk it (waking a task
 * unlinks it), so the node count is taken FIRST and used as the loop
 * bound, and each node's successor is saved before that node can be
 * removed. Must be called with interrupts masked.
 * ================================================================ */
void rtos__tick_advance(uint32_t n)
{
    if (n == 0u) { return; }
    g_ticks += n;

    tcb_t *t = delayed_head;
    if (t == 0) { return; }

    uint32_t remaining = 1u;
    for (tcb_t *it = t->next; it != t && it != 0; it = it->next) {
        remaining++;
    }

    while (remaining-- > 0u && t != 0) {
        tcb_t *nxt = t->next;          /* save before t can be unlinked */
        if (t->delay > n) {
            t->delay -= n;
        } else {
            t->delay = 0;
            list_remove(&delayed_head, t);
            t->state = TASK_READY;
            ready_add(t);
        }
        t = nxt;
    }
}

void rtos_tick_handler(void)
{
    uint32_t t0 = raw_ticker();
    uint32_t pm = enter_critical();

    rtos__tick_advance(1u);

    /* Round-robin: give the next same-priority task a turn. */
    if (rtos_current_task != 0) {
        uint32_t p = rtos_current_task->priority;
        if (ready_head[p] != 0) {
            ready_head[p] = ready_head[p]->next;
        }
    }

    rtos_schedule();
    /* Account this handler's IRQ-disabled window toward the longest
     * critical section observed. The empirical basis for the
     * interrupt-latency bound. Measured on the RAW sub-tick clock,
     * not rtos_cycles(): this window straddles rtos__tick_advance(),
     * which increments g_ticks, and rtos_cycles() carries a
     * g_ticks * period term, so a delta taken across it would report
     * a whole spurious tick every time. */
    uint32_t dt;
    if (raw_delta(t0, raw_ticker(), &dt)) {
        /* The genuine handler window is a few hundred cycles. Anything
         * at or beyond a full tick cannot be one, and is rejected. */
        if (dt < RTOS_SYSTICK_RELOAD && dt > g_max_irq_off) {
            g_max_irq_off = dt;
        }
    }
    exit_critical(pm);
}

/* SysTick_Handler. The RTOS system clock (overrides weak startup). */
void SysTick_Handler(void)
{
    rtos_tick_handler();
}

/* ================================================================
 * SysTick control surface used by poll.c
 *
 * rtos__systick_irq() toggles TICKINT *only*. ENABLE is deliberately
 * left set so the 24-bit counter keeps running with the interrupt
 * masked: that is what lets a poll section suppress preemption
 * without losing real time, and what lets the kernel reconstruct the
 * suppressed ticks afterwards by watching the counter wrap.
 * ================================================================ */
void rtos__systick_irq(int enable)
{
    if (enable) {
        SYSTICK_CTRL |= SYSTICK_CTRL_TICKINT;
    } else {
        SYSTICK_CTRL &= ~SYSTICK_CTRL_TICKINT;
    }
    rtos_dsb();
    rtos_isb();
}

uint32_t rtos__systick_val(void)    { return SYSTICK_VAL; }
uint32_t rtos__systick_reload(void) { return SYSTICK_LOAD; }

/* ================================================================
 * Cycle counter: DWT->CYCCNT on real hardware; a SysTick-derived
 * monotonic counter as a fallback (QEMU's Cortex-M model does not
 * implement the DWT cycle counter, but SysTick VAL does advance).
 * This keeps all timing code working for functional verification on
 * QEMU while remaining cycle-accurate on the STM32F4 Discovery board.
 * ================================================================ */
void rtos_cyccnt_enable(void)
{
    CORE_DEMCR |= CORE_DEMCR_TRCENA;
    DWT_LAR     = 0xC5ACCE55UL;     /* unlock DWT on cores that need it */
    DWT_CYCCNT  = 0;
    DWT_CTRL   |= DWT_CTRL_CYCCNTENA;

    /* Probe: if the counter advances, DWT is real. */
    volatile uint32_t a = DWT_CYCCNT;
    for (volatile int i = 0; i < 8; i++) { __asm volatile ("nop"); }
    volatile uint32_t b = DWT_CYCCNT;
    g_dwt_ok = (b != a) ? 1u : 0u;
}

int rtos_dwt_available(void) { return (int)g_dwt_ok; }

uint32_t rtos_cycles(void)
{
    if (g_dwt_ok) {
        return DWT_CYCCNT;       /* real silicon: one load, monotonic */
    }

    /* ---- SysTick-derived fallback (QEMU implements no DWT) ------
     * SysTick counts DOWN from RELOAD to 0 and reloads, so the
     * position within a tick is (RELOAD - VAL), and every completed
     * tick is another (RELOAD + 1) cycles. g_ticks IS the count of
     * reloads: the tick ISR bumps it once per reload, and
     * rtos_poll_exit() adds back the reloads that occurred while it
     * had the tick interrupt masked (see poll.c). So this stays
     * consistent across poll sections.
     *
     * The re-read loop guards against a tick landing between the two
     * reads, which would pair a pre-reload g_ticks with a
     * post-reload VAL and jump the clock by a whole tick.
     *
     * KNOWN LIMITATION, QEMU ONLY: while a poll section is open the
     * tick ISR is masked, so this counter stalls for the length of
     * the section and catches up when the section closes. Code that
     * needs elapsed time INSIDE a section must not use it, poll.c
     * therefore keeps its own reload-counting clock, which is
     * independent of g_ticks (see poll_elapsed_from_systick()).
     * On hardware the question does not arise: g_dwt_ok is set and
     * DWT->CYCCNT is a free-running monotonic cycle counter that no
     * interrupt mask can stop. */
    uint32_t reload = RTOS_SYSTICK_RELOAD;
    uint32_t ticks, val;
    do {
        ticks = g_ticks;
        val   = SYSTICK_VAL;
    } while (ticks != g_ticks);
    return ticks * (reload + 1u) + (reload - val);
}

/* ================================================================
 * MPU-based stack-overflow guard.
 *
 * rtos_mpu_init() enables the MPU once with a privileged-default
 * background map (PRIVDEFENA), so all normal accesses keep working,
 * and turns on the MemManage fault. rtos_mpu_on_switch() then places
 * a 32-byte, no-access region at the bottom of the incoming task's
 * stack on every context switch: if that task's stack pointer ever
 * descends into the guard, the CPU raises a MemManage fault instead
 * of silently corrupting the adjacent task's memory.
 *
 * The whole feature is compiled in only with -DRTOS_ENABLE_MPU
 * (hardware builds). On the default QEMU build g_mpu_enabled stays 0
 * and both functions are no-ops, so the functional demo is unaffected.
 * ================================================================ */
static uint32_t g_mpu_enabled;

void rtos_mpu_init(void)
{
#ifdef RTOS_ENABLE_MPU
    if ((MPU_TYPE & 0xFF00u) == 0) {
        return;                       /* no MPU implemented */
    }
    SCB_SHCSR |= SCB_SHCSR_MEMFAULTENA;   /* enable MemManage fault */
    /* ENABLE | PRIVDEFENA: regions we do not define fall back to the
     * default privileged memory map, so ordinary code is unaffected. */
    MPU_CTRL   = (1u << 0) | (1u << 2);
    rtos_dsb();
    rtos_isb();
    g_mpu_enabled = 1u;
#endif
}

void rtos_mpu_on_switch(void)
{
    if (!g_mpu_enabled) {
        return;                       /* guard disabled: cheap no-op */
    }
    tcb_t *t = rtos_current_task;
    if (t == 0 || t->stack_base == 0) { return; }

    /* Region 7 = 32 bytes, no access, at the (32-byte aligned) base
     * of the incoming task's stack. RASR: SIZE field = log2(32)-1 = 4,
     * AP = 000 (no access, privileged or not), ENABLE = 1. */
    uint32_t base = (uint32_t)t->stack_base & ~0x1FUL;
    MPU_RNR  = 7u;
    MPU_RBAR = base | (1u << 4) | 7u;     /* VALID + region number 7 */
    MPU_RASR = (0u << 24) | (4u << 1) | 1u;
    rtos_dsb();
    rtos_isb();
}

/* ================================================================
 * Internal helpers shared with sync.c / spsc.c
 * (declared here, used across the kernel)
 * ================================================================ */
uint32_t rtos__enter_critical(void)       { return enter_critical(); }
void     rtos__exit_critical(uint32_t pm) { exit_critical(pm); }

void rtos__block_current_on(tcb_t **waitlist, void *object)
{
    /* A poll section must not block: the tick is masked and switches
     * are deferred, so there is nothing to wake us and nothing to
     * switch to. The system would simply stop. Rather than hang on
     * a contract violation, force the section closed (restoring the
     * tick) and count it, so the fault is visible in the poll report
     * instead of presenting as a dead board. */
    if (rtos__poll_lock) {
        rtos__poll_violation();
    }

    tcb_t *t = rtos_current_task;
    ready_remove(t);
    t->state       = TASK_BLOCKED;
    t->wait_object = object;
    list_insert_tail(waitlist, t);
    rtos_schedule();
}

/* Unblock the HIGHEST-priority waiter from a wait list; returns it. */
tcb_t *rtos__unblock_highest(tcb_t **waitlist)
{
    if (*waitlist == 0) {
        return 0;
    }
    tcb_t *best = *waitlist;
    tcb_t *it   = best->next;
    while (it != *waitlist) {
        if (it->priority > best->priority) {
            best = it;
        }
        it = it->next;
    }
    list_remove(waitlist, best);
    best->wait_object = 0;
    ready_add(best);
    return best;
}

void rtos__ready_move_priority(tcb_t *t, uint32_t new_priority)
{
    /* Move a READY task between bands (used by priority inheritance). */
    if (t->state == TASK_READY) {
        ready_remove(t);
        t->priority = new_priority;
        ready_add(t);
    } else {
        t->priority = new_priority;
    }
}
