/* ================================================================
 * rtos_internal.h. Kernel-private helpers shared between the
 * scheduler (rtos.c) and the sync / queue modules. Not part of the
 * public API.
 * ================================================================ */

#ifndef RTOS_INTERNAL_H
#define RTOS_INTERNAL_H

#include "rtos.h"

/* Critical section (PRIMASK save/restore). */
uint32_t rtos__enter_critical(void);
void     rtos__exit_critical(uint32_t primask);

/* Block the running task on a wait list and reschedule. */
void rtos__block_current_on(tcb_t **waitlist, void *object);

/* Remove and ready the highest-priority waiter; returns it (or 0). */
tcb_t *rtos__unblock_highest(tcb_t **waitlist);

/* Change a task's effective priority, moving lists if it is READY. */
void rtos__ready_move_priority(tcb_t *t, uint32_t new_priority);

/* ---- Hooks used by poll.c (poll-mode scheduling) ---------------
 * The poll module suppresses the scheduler tick for the duration of
 * a poll section and then replays the ticks that did not fire, so it
 * needs to drive the kernel time base directly.
 * ---------------------------------------------------------------- */

/* Advance the kernel time base by 'n' ticks in ONE pass: bump the
 * tick count and decrement every sleeper's remaining delay by n,
 * readying those that reach zero. Must be called with interrupts
 * masked. n == 0 is a no-op. */
void rtos__tick_advance(uint32_t n);

/* Enable (1) or disable (0) the SysTick *interrupt* only. The
 * counter keeps running either way, so no real time is lost. */
void rtos__systick_irq(int enable);

/* Current SysTick VAL (counts DOWN from RELOAD to 0) and the reload
 * value, i.e. cycles per tick minus one. */
uint32_t rtos__systick_val(void);
uint32_t rtos__systick_reload(void);

/* Poll-section interlock, read by rtos_schedule() in rtos.c.
 * Non-zero => a poll section is open: rtos_schedule() must record a
 * deferred switch instead of pending PendSV. Defined in poll.c. */
extern volatile uint32_t rtos__poll_lock;
extern volatile uint32_t rtos__poll_deferred;

/* Called by rtos__block_current_on() when a task tries to block
 * inside a poll section (a contract violation). Force-closes the
 * section so the system stays live and counts the violation. */
void rtos__poll_violation(void);

#endif /* RTOS_INTERNAL_H */
