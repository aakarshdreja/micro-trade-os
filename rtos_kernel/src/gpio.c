/* ================================================================
 * gpio.c: STM32F407 GPIOD bring-up for the jitter benchmark.
 * See gpio.h for why this is a direct-register driver and not a HAL.
 * ================================================================ */

#include "gpio.h"

/* ---- RCC (reset & clock control) ------------------------------- */
#define RCC_AHB1ENR   (*(volatile uint32_t *)(0x40023800UL + 0x30UL))
#define RCC_GPIODEN   (1UL << 3)

/* ---- GPIOD register block -------------------------------------- */
#define GPIOD_BASE    0x40020C00UL
#define GPIOD_MODER   (*(volatile uint32_t *)(GPIOD_BASE + 0x00UL))
#define GPIOD_OTYPER  (*(volatile uint32_t *)(GPIOD_BASE + 0x04UL))
#define GPIOD_OSPEEDR (*(volatile uint32_t *)(GPIOD_BASE + 0x08UL))
#define GPIOD_PUPDR   (*(volatile uint32_t *)(GPIOD_BASE + 0x0CUL))

static void gpio_config_output(uint32_t pin)
{
    /* MODER: 2 bits/pin, 01 = general-purpose output */
    GPIOD_MODER   = (GPIOD_MODER   & ~(3UL << (pin * 2u))) | (1UL << (pin * 2u));
    /* OTYPER: 1 bit/pin, 0 = push-pull (we want a driven edge both
     * ways; open-drain would make the rising edge depend on a pull-up
     * and add an RC time constant to the measurement). */
    GPIOD_OTYPER &= ~(1UL << pin);
    /* OSPEEDR: 2 bits/pin, 11 = very high speed, so the observed
     * edge rate is set by the pad, not by the slew limiter. */
    GPIOD_OSPEEDR |= (3UL << (pin * 2u));
    /* PUPDR: 00 = no pull-up/pull-down on a driven output. */
    GPIOD_PUPDR   &= ~(3UL << (pin * 2u));
}

void gpio_init(void)
{
    RCC_AHB1ENR |= RCC_GPIODEN;
    /* The reference manual requires a dummy read-back after enabling
     * a peripheral clock: the write is posted on the bus, and the
     * peripheral is not guaranteed ready until it has landed. */
    volatile uint32_t sync = RCC_AHB1ENR;
    (void)sync;

    gpio_config_output(GPIO_PIN_SIGNAL);
    gpio_config_output(GPIO_PIN_MARKER);

    gpio_clear(GPIO_PIN_SIGNAL);
    gpio_clear(GPIO_PIN_MARKER);
}
