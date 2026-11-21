/* ================================================================
 * gpio.h: Minimal STM32F4 GPIO driver for the jitter benchmark
 *
 * The jitter experiment has to be observable from OUTSIDE the chip:
 * the whole point is that a logic analyser sees a square wave whose
 * edges are evenly spaced, and that the on-chip cycle counts agree
 * with what the scope shows. This driver exists to make that one pin
 * toggle as cheaply and as deterministically as possible.
 *
 * Deliberately not a HAL. Two design points matter for jitter:
 *
 *  - Writes go through BSRR, not ODR. BSRR is write-only and
 *    set/reset-per-bit, so a pin change is a single store with no
 *    read-modify-write and no interaction with the other pins of the
 *    port. An ODR-based toggle (read, XOR, write) is three accesses
 *    and its duration depends on bus state.
 *  - The pin is configured at maximum output speed so the edge rate
 *    is a property of the pad, not of the driver.
 *
 * Pins used (chosen per board in board.h):
 *    Discovery : PD12 (green LED), PD13 (orange LED), on the header
 *    Black Pill: PB12, PB13, on the edge header
 *  The first is the toggled signal under test; the second is a marker,
 *  high for the whole measured window.
 * ================================================================ */

#ifndef RTOS_GPIO_H
#define RTOS_GPIO_H

#include <stdint.h>
#include "board.h"

#define GPIO_PIN_SIGNAL   BOARD_PIN_SIGNAL   /* square wave under test     */
#define GPIO_PIN_MARKER   BOARD_PIN_MARKER   /* measurement-window marker  */

/* Enable the probe port's clock and configure the two pins as fast
 * outputs. */
void gpio_init(void);

/* Single-store, read-modify-write-free pin control via BSRR.
 * BSRR[15:0] set a pin, BSRR[31:16] clear it. Marked always_inline
 * so the benchmark's inner loop contains the store itself rather
 * than a call. A call would add its own fixed cost to every edge
 * and blunt exactly what we are trying to measure. */
#define GPIO_PROBE_BSRR  (*(volatile uint32_t *)(BOARD_GPIO_BASE + 0x18UL))

__attribute__((always_inline))
static inline void gpio_set(uint32_t pin)
{
    GPIO_PROBE_BSRR = (1UL << pin);
}

__attribute__((always_inline))
static inline void gpio_clear(uint32_t pin)
{
    GPIO_PROBE_BSRR = (1UL << (pin + 16u));
}

#endif /* RTOS_GPIO_H */
