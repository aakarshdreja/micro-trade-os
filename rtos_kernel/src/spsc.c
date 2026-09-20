/* ================================================================
 * spsc.c: Lock-free, wait-free single-producer/single-consumer
 * ring buffer for hot-path inter-task communication.
 *
 * Only one task ever produces and one task ever consumes a given
 * queue, so no mutual exclusion is required. Progress is wait-free:
 * try_send / try_recv complete in a bounded number of instructions
 * with no locks and no interrupt masking.
 *
 * Correctness rests on memory ordering. On Cortex-M4 (single core,
 * in-order-ish but with a store buffer) the producer MUST make the
 * item data globally visible BEFORE it publishes the incremented
 * tail; otherwise the consumer could observe the new index and read
 * stale data. A DMB (data memory barrier) enforces that order. The
 * consumer symmetrically issues a DMB after reading head/data before
 * publishing the advanced head.
 *
 * head/tail are free-running monotonic counters; the physical slot
 * is index & mask. capacity is a power of two so mask = capacity-1.
 * The difference (tail - head) is the occupancy and never exceeds
 * capacity, so unsigned wraparound of the counters is harmless.
 * ================================================================ */

#include "rtos.h"

static void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

void rtos_spsc_init(spsc_t *q, void *storage, uint32_t item_size,
                    uint32_t capacity_pow2)
{
    q->buffer    = (uint8_t *)storage;
    q->item_size = item_size;
    q->capacity  = capacity_pow2;
    q->mask      = capacity_pow2 - 1u;   /* assumes power of two */
    q->head      = 0;
    q->tail      = 0;
}

uint32_t rtos_spsc_count(const spsc_t *q)
{
    return q->tail - q->head;
}

/* Producer side (called by exactly one task). */
int rtos_spsc_try_send(spsc_t *q, const void *item)
{
    uint32_t tail = q->tail;
    uint32_t head = q->head;          /* single read of consumer index */

    if ((tail - head) >= q->capacity) {
        return 0;                      /* full */
    }

    uint32_t slot = tail & q->mask;
    copy_bytes(q->buffer + slot * q->item_size,
               (const uint8_t *)item, q->item_size);

    rtos_dmb();                        /* data visible BEFORE index */
    q->tail = tail + 1u;               /* publish */
    return 1;
}

/* Consumer side (called by exactly one task). */
int rtos_spsc_try_recv(spsc_t *q, void *item)
{
    uint32_t head = q->head;
    uint32_t tail = q->tail;           /* single read of producer index */

    if (tail == head) {
        return 0;                      /* empty */
    }

    rtos_dmb();                        /* order index read before data read */
    uint32_t slot = head & q->mask;
    copy_bytes((uint8_t *)item,
               q->buffer + slot * q->item_size, q->item_size);

    rtos_dmb();                        /* data consumed BEFORE index publish */
    q->head = head + 1u;               /* publish */
    return 1;
}

/* ================================================================
 * CACHE-ALIGNED VARIANT (spsc_ca_t)
 * ================================================================
 * Algorithmically identical to the ring above. Two changes carry the
 * performance argument; see the struct definition in rtos.h for the
 * layout and the reasoning.
 *
 *  1. Cache-line partitioning. Producer-owned state (tail,
 *     cached_head) and consumer-owned state (head, cached_tail) live
 *     in different RTOS_CACHE_LINE-sized regions, so neither side
 *     ever dirties a line the other is reading. On a cached core
 *     that removes false sharing; on the cache-less Cortex-M4 it
 *     costs nothing and keeps the structure correct when the same
 *     source is compiled for a Cortex-M7.
 *
 *  2. Shadow indices. try_send consults the producer's own stale
 *     copy of head (cached_head) to test for "full", and touches the
 *     consumer's real index ONLY when that copy says the ring is
 *     full. try_recv mirrors it. On a queue that is kept drained, 
 *     the case that matters on a hot path. The peer's index is
 *     never read at all.
 *
 * Point 2 is what the IPC benchmark measures on this part: it
 * removes a load and a dependent compare from the common path of
 * every send and every receive. Point 1 is a portability and
 * multi-core property, not an M4 speed-up, and is not claimed as one.
 * ================================================================ */

int rtos_spsc_ca_init(spsc_ca_t *q, void *storage, uint32_t item_size,
                      uint32_t capacity_pow2)
{
    /* Capacity must be a power of two: the index-to-slot mapping is
     * a mask, and the monotonic-counter arithmetic relies on it. */
    if (capacity_pow2 == 0u || (capacity_pow2 & (capacity_pow2 - 1u)) != 0u) {
        return 0;
    }
    /* The payload array must start on a cache-line boundary, or slot
     * 0 shares a line with whatever precedes it, the exact effect
     * the alignment exists to prevent. Declare the storage with
     * RTOS_CACHE_ALIGN. */
    if (((uint32_t)storage & (RTOS_CACHE_LINE - 1u)) != 0u) {
        return 0;
    }

    q->buffer      = (uint8_t *)storage;
    q->item_size   = item_size;
    q->capacity    = capacity_pow2;
    q->mask        = capacity_pow2 - 1u;
    q->tail        = 0;
    q->cached_head = 0;
    q->head        = 0;
    q->cached_tail = 0;
    return 1;
}

uint32_t rtos_spsc_ca_count(const spsc_ca_t *q)
{
    return q->tail - q->head;
}

/* Producer side (called by exactly one task). */
int rtos_spsc_ca_try_send(spsc_ca_t *q, const void *item)
{
    uint32_t tail = q->tail;                 /* our own line         */

    /* Fast path: decide on the SHADOW copy of the consumer index. */
    if ((tail - q->cached_head) >= q->capacity) {
        /* Shadow says full. Only now pay to read the consumer's
         * line, and refresh the shadow while we are there. */
        q->cached_head = q->head;
        if ((tail - q->cached_head) >= q->capacity) {
            return 0;                        /* genuinely full       */
        }
    }

    uint32_t slot = tail & q->mask;
    copy_bytes(q->buffer + slot * q->item_size,
               (const uint8_t *)item, q->item_size);

    rtos_dmb();                              /* data visible BEFORE index */
    q->tail = tail + 1u;                     /* publish              */
    return 1;
}

/* Consumer side (called by exactly one task). */
int rtos_spsc_ca_try_recv(spsc_ca_t *q, void *item)
{
    uint32_t head = q->head;                 /* our own line         */

    /* Fast path: decide on the SHADOW copy of the producer index. */
    if (head == q->cached_tail) {
        q->cached_tail = q->tail;            /* shadow says empty    */
        if (head == q->cached_tail) {
            return 0;                        /* genuinely empty      */
        }
    }

    rtos_dmb();                              /* order index read before data */
    uint32_t slot = head & q->mask;
    copy_bytes((uint8_t *)item,
               q->buffer + slot * q->item_size, q->item_size);

    rtos_dmb();                              /* data consumed BEFORE publish */
    q->head = head + 1u;                     /* publish              */
    return 1;
}
