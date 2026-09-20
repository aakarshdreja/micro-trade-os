/* ================================================================
 * queue_baseline.c. The CONTROL in the IPC experiment
 *
 * A conventional copying queue, built the way a classical RTOS
 * builds one. FreeRTOS's xQueueSend/xQueueReceive do, in order:
 *
 *   enter a critical section (or take the queue lock)
 *   memcpy the item into the ring
 *   update the indices
 *   leave the critical section
 *   signal the waiting side
 *
 * and the receive path is the mirror image. That is what this file
 * reproduces on top of this kernel's own mutex and semaphores:
 * a priority-inheriting mutex for mutual exclusion, a counting
 * semaphore for occupancy and another for free space.
 *
 * WHY IT EXISTS
 * -------------
 * The claim under test is that a lock-free ring beats a lock-based
 * queue on the hot path. A fair test needs the two to differ in ONE
 * variable, the synchronisation strategy, and to be identical in
 * every other: same compiler, same -O2, same clock, same message
 * size, same kernel, same measurement harness. Quoting a FreeRTOS
 * figure from another project's paper would confound all of those at
 * once. So the baseline is built here, deliberately, and is NOT
 * strawmanned: it uses the same tight copy loop as the lock-free
 * ring, and its fast path (non-blocking try_send/try_recv) skips the
 * semaphores entirely, which is the most favourable honest version
 * of the design.
 *
 * The cost that remains is the cost the experiment is about: taking
 * and releasing a lock, and the critical section that comes with it.
 * ================================================================ */

#include "rtos.h"
#include "rtos_internal.h"

static void qb_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

void rtos_queue_init(rtos_queue_t *q, void *storage, uint32_t item_size,
                     uint32_t capacity)
{
    q->buffer    = (uint8_t *)storage;
    q->item_size = item_size;
    q->capacity  = capacity;
    q->head      = 0;
    q->tail      = 0;
    q->count     = 0;
    rtos_mutex_init(&q->lock);
    rtos_sem_init(&q->items, 0, (int32_t)capacity);
    rtos_sem_init(&q->space, (int32_t)capacity, (int32_t)capacity);
}

/* ---- Non-blocking fast path ------------------------------------
 * Lock, copy, unlock. This is the path the benchmark measures, and
 * it is the cheapest correct version of a lock-based queue: no
 * semaphore traffic at all when the caller does not need to wait.
 * ---------------------------------------------------------------- */
int rtos_queue_try_send(rtos_queue_t *q, const void *item)
{
    rtos_mutex_lock(&q->lock);

    if (q->count >= q->capacity) {
        rtos_mutex_unlock(&q->lock);
        return 0;                       /* full */
    }
    qb_copy(q->buffer + q->tail * q->item_size,
            (const uint8_t *)item, q->item_size);
    q->tail = (q->tail + 1u) % q->capacity;
    q->count++;

    rtos_mutex_unlock(&q->lock);
    return 1;
}

int rtos_queue_try_recv(rtos_queue_t *q, void *item)
{
    rtos_mutex_lock(&q->lock);

    if (q->count == 0u) {
        rtos_mutex_unlock(&q->lock);
        return 0;                       /* empty */
    }
    qb_copy((uint8_t *)item,
            q->buffer + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1u) % q->capacity;
    q->count--;

    rtos_mutex_unlock(&q->lock);
    return 1;
}

/* ---- Blocking path ---------------------------------------------
 * The full classical semantics: wait for space, lock, copy, unlock,
 * signal the consumer. Provided for completeness. The demo uses it
 * to show the baseline queue working as a real queue, not only as a
 * benchmark subject.
 * ---------------------------------------------------------------- */
void rtos_queue_send(rtos_queue_t *q, const void *item)
{
    rtos_sem_take(&q->space);           /* wait for a free slot */

    rtos_mutex_lock(&q->lock);
    qb_copy(q->buffer + q->tail * q->item_size,
            (const uint8_t *)item, q->item_size);
    q->tail = (q->tail + 1u) % q->capacity;
    q->count++;
    rtos_mutex_unlock(&q->lock);

    rtos_sem_give(&q->items);           /* wake a waiting consumer */
}

void rtos_queue_recv(rtos_queue_t *q, void *item)
{
    rtos_sem_take(&q->items);           /* wait for an item */

    rtos_mutex_lock(&q->lock);
    qb_copy((uint8_t *)item,
            q->buffer + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1u) % q->capacity;
    q->count--;
    rtos_mutex_unlock(&q->lock);

    rtos_sem_give(&q->space);           /* wake a waiting producer */
}
