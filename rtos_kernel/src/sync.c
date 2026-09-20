/* ================================================================
 * sync.c: Synchronisation primitives
 *
 *   - Binary / counting semaphores
 *   - Mutex with PRIORITY INHERITANCE
 *
 * Priority inheritance is the property that matters for HFT: if a
 * low-priority housekeeping task holds a lock that a high-priority
 * order-dispatch task needs, the owner is temporarily boosted to the
 * blocker's priority so it can finish quickly and release the lock, 
 * eliminating unbounded priority inversion.
 * ================================================================ */

#include "rtos.h"
#include "rtos_internal.h"

/* ================================================================
 * Semaphore
 * ================================================================ */
void rtos_sem_init(rtos_sem_t *s, int32_t initial, int32_t max_count)
{
    s->count     = initial;
    s->max_count = max_count;
    s->waiters   = 0;
}

int rtos_sem_try_take(rtos_sem_t *s)
{
    int got = 0;
    uint32_t pm = rtos__enter_critical();
    if (s->count > 0) {
        s->count--;
        got = 1;
    }
    rtos__exit_critical(pm);
    return got;
}

void rtos_sem_take(rtos_sem_t *s)
{
    uint32_t pm = rtos__enter_critical();
    while (s->count == 0) {
        /* No token: block until a give() wakes us, then re-check. */
        rtos__block_current_on(&s->waiters, s);
        rtos__exit_critical(pm);      /* PendSV switches us out here */
        pm = rtos__enter_critical();  /* resumed: re-evaluate        */
    }
    s->count--;
    rtos__exit_critical(pm);
}

void rtos_sem_give(rtos_sem_t *s)
{
    uint32_t pm = rtos__enter_critical();
    if (s->max_count == 0 || s->count < s->max_count) {
        s->count++;
    }
    tcb_t *woken = rtos__unblock_highest(&s->waiters);
    if (woken != 0) {
        rtos_schedule();   /* woken task may outrank us -> switch */
    }
    rtos__exit_critical(pm);
}

/* ISR-safe give: no rescheduling call needed here, the SysTick
 * handler calls rtos_schedule() on its way out anyway. */
void rtos_sem_give_from_isr(rtos_sem_t *s)
{
    uint32_t pm = rtos__enter_critical();
    if (s->max_count == 0 || s->count < s->max_count) {
        s->count++;
    }
    (void)rtos__unblock_highest(&s->waiters);
    rtos__exit_critical(pm);
}

/* ================================================================
 * Mutex with priority inheritance
 * ================================================================ */
void rtos_mutex_init(rtos_mutex_t *m)
{
    m->owner            = 0;
    m->waiters          = 0;
    m->owner_saved_prio = 0;
}

void rtos_mutex_lock(rtos_mutex_t *m)
{
    uint32_t pm = rtos__enter_critical();

    if (m->owner == 0) {
        /* Free: take it. */
        m->owner            = rtos_current();
        m->owner_saved_prio = m->owner->priority;
        rtos_current()->held_mutex = m;
        rtos__exit_critical(pm);
        return;
    }

    /* Held: apply priority inheritance if we outrank the owner, then
     * block until the owner releases and hands the mutex to us. */
    for (;;) {
        tcb_t *self  = rtos_current();
        tcb_t *owner = m->owner;

        if (owner != 0 && self->priority > owner->priority) {
            rtos__ready_move_priority(owner, self->priority);
        }

        rtos__block_current_on(&m->waiters, m);
        rtos__exit_critical(pm);       /* switch away */
        pm = rtos__enter_critical();

        /* Unlock() hands ownership directly to the woken task. */
        if (m->owner == rtos_current()) {
            break;
        }
    }

    rtos__exit_critical(pm);
}

void rtos_mutex_unlock(rtos_mutex_t *m)
{
    uint32_t pm = rtos__enter_critical();
    tcb_t *self = rtos_current();

    if (m->owner != self) {
        rtos__exit_critical(pm);   /* not the owner, ignore */
        return;
    }

    /* Restore our base priority if we were boosted by inheritance. */
    if (self->priority != m->owner_saved_prio) {
        rtos__ready_move_priority(self, self->base_priority);
    }
    self->held_mutex = 0;

    /* Hand the mutex directly to the highest-priority waiter. */
    tcb_t *next_owner = rtos__unblock_highest(&m->waiters);
    if (next_owner != 0) {
        m->owner            = next_owner;
        m->owner_saved_prio = next_owner->priority;
        next_owner->held_mutex = m;
        rtos_schedule();
    } else {
        m->owner = 0;
    }

    rtos__exit_critical(pm);
}
