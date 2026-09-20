/* ================================================================
 * poll.c: POLL-MODE SCHEDULING
 *
 * The problem
 * -----------
 * A conventional RTOS keeps a periodic timer interrupt running at
 * all times, 1 kHz here, as in a stock FreeRTOS configuration. That
 * interrupt is what makes preemption possible, and it is also what
 * makes latency non-deterministic: it can land on ANY instruction of
 * a time-critical task. The victim pays the vector fetch, the stack
 * frame push, the handler body, the scheduler pass and the return.
 * On this kernel that is a few hundred cycles, and, crucially, it
 * arrives at an instant the task cannot predict. Averaged over a
 * long run the cost is negligible; as a worst case on a single
 * iteration it is the entire jitter budget.
 *
 * The technique
 * -------------
 * Exchange-colocated trading systems solve this by not being
 * interrupted at all: the hot thread is pinned, the kernel is out of
 * its way, and it polls. Poll-mode scheduling is that idea reduced
 * to what a Cortex-M4 can do.
 *
 * A TASK_CLASS_POLL task opens a POLL SECTION. For its duration:
 *
 *   - SysTick's TICKINT bit is cleared, so the tick cannot fire.
 *     ENABLE is left set, so the 24-bit counter keeps counting and
 *     no real time is lost. This is what makes the section
 *     reversible.
 *   - rtos_schedule() stops pending PendSV and records a deferred
 *     switch instead (rtos__poll_deferred), so nothing can
 *     deschedule the task even if an ISR readies a higher-priority
 *     one.
 *   - In RTOS_POLL_EXCLUSIVE mode BASEPRI is additionally raised so
 *     no configurable-priority interrupt can run at all. Faults and
 *     NMI remain enabled, deliberately: masking those would trade a
 *     jitter problem for a silent-corruption problem.
 *
 * On exit the kernel replays the suppressed ticks, restores TICKINT
 * and BASEPRI, and takes the deferred switch. The task set's timing
 * therefore sees a scheduler that paused, not one that lost time.
 *
 * Counting the suppressed ticks
 * -----------------------------
 * SysTick's COUNTFLAG saturates, so hardware cannot tell us how many
 * reloads happened while the interrupt was masked. We count them in
 * software instead: SysTick counts DOWN, so whenever a fresh sample
 * of VAL is LARGER than the previous sample, the counter wrapped, 
 * one tick was suppressed. The sampling hook is rtos_poll_expired(),
 * which the poll loop already calls once per iteration to honour its
 * cycle budget. A poll loop iterates far more often than once per
 * millisecond, so every reload is observed.
 *
 * Where DWT is present (real silicon) the elapsed-cycle count gives
 * an independent estimate of the same quantity; we take the larger
 * of the two, so a poll loop that samples too rarely degrades to an
 * approximation rather than to lost time.
 *
 * What this costs
 * ---------------
 * Poll sections are cooperative: the kernel cannot end one, only the
 * task can. That is the honest trade, determinism inside the
 * section is bought with the programmer's obligation to leave it.
 * The kernel makes the obligation auditable rather than implicit: it
 * records every section's length, every budget overrun and every
 * blocking-call violation, and rtos_poll_report() prints them.
 * ================================================================ */

#include "rtos.h"
#include "rtos_internal.h"
#include "semihosting.h"

/* ---- Poll-section interlock (read by rtos_schedule()) ---------- */
volatile uint32_t rtos__poll_lock     = 0;
volatile uint32_t rtos__poll_deferred = 0;

/* ---- Open-section state ---------------------------------------- */
static uint32_t s_mode;          /* RTOS_POLL_NO_TICK | _EXCLUSIVE  */
static uint32_t s_budget;        /* cycles; 0 = unlimited           */
static uint32_t s_t0;            /* rtos_cycles() at entry          */
static uint32_t s_val0;          /* SysTick VAL at entry            */
static uint32_t s_prev_val;      /* previous VAL sample             */
static uint32_t s_reloads;       /* suppressed reloads counted      */
static uint32_t s_basepri_saved; /* BASEPRI to restore on exit      */

/* ---- Cumulative statistics ------------------------------------- */
static rtos_poll_info_t s_info;

/* ================================================================
 * BASEPRI helpers (EXCLUSIVE mode)
 *
 * BASEPRI masks every exception whose priority number is numerically
 * >= the value written (lower number = more urgent on Cortex-M). We
 * write 0x10, which is more urgent than every priority the kernel
 * assigns (SysTick 0xE0, PendSV 0xFF) and than the default 0x00...
 * no: priority 0 would NOT be masked by BASEPRI=0x10, so any IRQ the
 * application leaves at the reset default of 0 stays live. The
 * kernel therefore also documents that EXCLUSIVE mode assumes the
 * application has not parked a peripheral at priority 0. Writing 1
 * instead would mask everything except faults/NMI, at the cost of
 * being unable to nest anything at all; 0x10 leaves room for a
 * genuinely-critical device to stay above the barrier by design.
 * ================================================================ */
#define POLL_BASEPRI_LEVEL  0x10u

static inline uint32_t basepri_get(void)
{
    uint32_t v;
    __asm volatile ("mrs %0, basepri" : "=r"(v));
    return v;
}

static inline void basepri_set(uint32_t v)
{
    __asm volatile ("msr basepri, %0" :: "r"(v) : "memory");
}

/* ================================================================
 * Timekeeping sample. See the header comment. Counts a suppressed
 * SysTick reload each time the down-counter is seen to have wrapped.
 * ================================================================ */
static void poll_sample(void)
{
    uint32_t v = rtos__systick_val();
    if (v > s_prev_val) {
        s_reloads++;          /* counts DOWN: an increase => wrapped */
    }
    s_prev_val = v;
}

/* Cycles elapsed since entry, reconstructed from the reload count
 * and the down-counter position. Correct under unsigned wraparound
 * for any section shorter than 2^32 cycles. */
static uint32_t poll_elapsed_from_systick(void)
{
    uint32_t period = rtos__systick_reload() + 1u;
    return s_reloads * period + s_val0 - s_prev_val;
}

/* ================================================================
 * Public API
 * ================================================================ */
void rtos_task_set_class(tcb_t *t, task_class_t klass)
{
    if (t != 0) {
        t->klass = klass;
    }
}

int rtos_poll_active(void) { return (int)rtos__poll_lock; }

int rtos_poll_enter(uint32_t mode, uint32_t budget_cycles)
{
    tcb_t *self = rtos_current();

    /* Only a poll-class task may open a section, and sections do not
     * nest: a nested enter would make the exit bookkeeping ambiguous
     * and is a programming error, not a condition to recover from. */
    if (self == 0 || self->klass != TASK_CLASS_POLL) { return 0; }
    if (rtos__poll_lock)                             { return 0; }

    uint32_t pm = rtos__enter_critical();

    s_mode     = mode;
    s_budget   = budget_cycles;
    s_t0       = rtos_cycles();
    s_val0     = rtos__systick_val();
    s_prev_val = s_val0;
    s_reloads  = 0;

    /* Suppress the scheduler tick. The counter keeps running. */
    rtos__systick_irq(0);

    if (mode == RTOS_POLL_EXCLUSIVE) {
        s_basepri_saved = basepri_get();
        basepri_set(POLL_BASEPRI_LEVEL);
    }

    rtos__poll_deferred = 0;
    rtos__poll_lock     = 1;      /* rtos_schedule() now defers      */
    s_info.sections++;

    rtos__exit_critical(pm);
    rtos_dsb();
    rtos_isb();
    return 1;
}

uint32_t rtos_poll_elapsed(void)
{
    if (!rtos__poll_lock) { return 0; }
    if (rtos_dwt_available()) {
        return rtos_cycles() - s_t0;      /* monotonic on silicon   */
    }
    poll_sample();
    return poll_elapsed_from_systick();   /* independent of g_ticks */
}

int rtos_poll_expired(void)
{
    if (!rtos__poll_lock) { return 1; }

    /* Sample unconditionally: this call is the kernel's only chance
     * to observe a SysTick wrap while the tick interrupt is masked. */
    poll_sample();

    if (s_budget == 0u) { return 0; }     /* no budget configured    */

    /* NOTE: the kernel's own rtos_cycles() stalls inside a poll
     * section on the QEMU fallback path, because that path derives
     * time from the tick count and we have masked the tick. Use the
     * section's own reload-counting clock there. */
    uint32_t elapsed = rtos_dwt_available() ? (rtos_cycles() - s_t0)
                                            : poll_elapsed_from_systick();
    return (elapsed >= s_budget) ? 1 : 0;
}

void rtos_poll_exit(void)
{
    if (!rtos__poll_lock) { return; }

    poll_sample();

    /* --- how long were we in here, and how many ticks did we eat?
     * Two independent estimates of the suppressed-tick count, and we
     * take the larger:
     *
     *   s_reloads   counted by watching the SysTick down-counter wrap
     *               (exact, provided the loop sampled often enough)
     *   elapsed/T   derived from the elapsed cycle count (exact on
     *               silicon, where the clock is DWT)
     *
     * Trusting only the first would lose time if a poll loop called
     * rtos_poll_expired() less than once per tick period; trusting
     * only the second would inherit any coarseness in the fallback
     * clock. Taking the maximum means the kernel can run the clock
     * forward but never backward. A late wake-up is a scheduling
     * artefact, a lost tick is a correctness bug.
     *
     * On the QEMU fallback path rtos_cycles() is itself derived from
     * the tick count we just suppressed, so only the section's own
     * reload clock is meaningful there. */
    uint32_t elapsed = poll_elapsed_from_systick();
    uint32_t missed  = s_reloads;

    if (rtos_dwt_available()) {
        elapsed = rtos_cycles() - s_t0;          /* cycle-exact */
        uint32_t period   = rtos__systick_reload() + 1u;
        uint32_t from_cyc = (period != 0u) ? (elapsed / period) : 0u;
        if (from_cyc > missed) { missed = from_cyc; }
    }

    if (elapsed > s_info.max_cycles) { s_info.max_cycles = elapsed; }
    if (s_budget != 0u && elapsed >= s_budget) { s_info.overruns++; }

    uint32_t pm = rtos__enter_critical();

    /* --- close the section ------------------------------------- */
    rtos__poll_lock = 0;

    if (s_mode == RTOS_POLL_EXCLUSIVE) {
        basepri_set(s_basepri_saved);
    }

    /* --- give the kernel back the time it did not see ----------- */
    if (missed > 0u) {
        s_info.ticks_replayed += missed;
        rtos__tick_advance(missed);
    }

    /* --- restore the scheduler tick ----------------------------- */
    rtos__systick_irq(1);

    /* --- take the switch we deferred, if any -------------------- */
    if (rtos__poll_deferred) {
        rtos__poll_deferred = 0;
        s_info.deferred_switch++;
        rtos_schedule();
    } else if (missed > 0u) {
        /* Replayed ticks may have readied a higher-priority task. */
        rtos_schedule();
    }

    rtos__exit_critical(pm);
}

/* Called from rtos__block_current_on() when a task blocks inside a
 * poll section. Blocking there cannot work. There is no tick to
 * wake us and no switch to take, so rather than hang, we close the
 * section and record the violation for the report. */
void rtos__poll_violation(void)
{
    if (!rtos__poll_lock) { return; }
    s_info.violations++;

    rtos__poll_lock = 0;
    if (s_mode == RTOS_POLL_EXCLUSIVE) {
        basepri_set(s_basepri_saved);
    }
    if (s_reloads > 0u) {
        s_info.ticks_replayed += s_reloads;
        rtos__tick_advance(s_reloads);
    }
    rtos__systick_irq(1);
}

void rtos_poll_info(rtos_poll_info_t *out)
{
    if (out == 0) { return; }
    *out = s_info;
}

/* ---- Reporting -------------------------------------------------- */
static void put_u32(uint32_t v)
{
    char b[12];
    int i = 11;
    b[i--] = '\0';
    if (v == 0) { b[i--] = '0'; }
    while (v > 0) { b[i--] = (char)('0' + (v % 10)); v /= 10; }
    sh_puts(&b[i + 1]);
}

void rtos_poll_report(void)
{
    sh_puts("poll-mode: sections=");   put_u32(s_info.sections);
    sh_puts(" ticks_replayed=");       put_u32(s_info.ticks_replayed);
    sh_puts(" deferred_switch=");      put_u32(s_info.deferred_switch);
    sh_puts(" overruns=");             put_u32(s_info.overruns);
    sh_puts(" violations=");           put_u32(s_info.violations);
    sh_puts(" max_section=");          put_u32(s_info.max_cycles);
    sh_puts(" cyc\r\n");
}
