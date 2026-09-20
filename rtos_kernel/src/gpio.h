/* ================================================================
 * gpio.h: Minimal STM32F407 GPIO driver for the jitter benchmark
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
 * Pins used (STM32F4 Discovery on-board LEDs, brought out on the
 * header so a probe can reach them):
 *    PD12 green. The toggled signal under test
 *    PD13 orange. A marker: high for the whole measured window
 * ================================================================ */

#ifndef RTOS_GPIO_H
#define RTOS_GPIO_H

#include <stdint.h>

#define GPIO_PIN_SIGNAL   12u    /* PD12. Square wave under test */
#define GPIO_PIN_MARKER   13u    /* PD13, measurement-window marker */

/* Enable GPIOD's clock and configure the two pins as fast outputs. */
void gpio_init(void);

/* Single-store, read-modify-write-free pin control via BSRR.
 * BSRR[15:0] set a pin, BSRR[31:16] clear it. Marked always_inline
 * so the benchmark's inner loop contains the store itself rather
 * than a call. A call would add its own fixed cost to every edge
 * and blunt exactly what we are trying to measure. */
#define GPIOD_BSRR  (*(volatile uint32_t *)(0x40020C00UL + 0x18UL))

__attribute__((always_inline))
static inline void gpio_set(uint32_t pin)
{
    GPIOD_BSRR = (1UL << pin);
}

__attribute__((always_inline))
static inline void gpio_clear(uint32_t pin)
{
    GPIOD_BSRR = (1UL << (pin + 16u));
}

#endif /* RTOS_GPIO_H */
