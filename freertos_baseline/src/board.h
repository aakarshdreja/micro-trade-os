/* ================================================================
 * board.h: Per-board constants for hardware builds
 *
 * The kernel itself only touches Cortex-M4 core registers, which are
 * the same on every STM32F4. What differs between boards is the
 * clock tree, the Flash size, and which GPIO pins are free for the
 * jitter probe. Those live here, selected by the Makefile:
 *
 *   make ... HW=1 BOARD=disc       -> BOARD_DISC_F407     (default)
 *   make ... HW=1 BOARD=blackpill  -> BOARD_BLACKPILL_F411
 *
 * QEMU builds (no HW=1) define neither and use the Discovery pins,
 * which is what the QEMU olimex model has always been run with.
 * ================================================================ */

#ifndef RTOS_BOARD_H
#define RTOS_BOARD_H

#include <stdint.h>

#if defined(BOARD_BLACKPILL_F411)
/* WeAct Black Pill V3.x, STM32F411CEU6: 25 MHz HSE, 100 MHz core.
 * The UFQFPN48 package has no port D, so the probe pins move to
 * PB12/PB13, which are on the board's edge header. */
#  define BOARD_NAME          "WeAct Black Pill STM32F411CEU6"
#  define BOARD_GPIO_BASE     0x40020400UL     /* GPIOB          */
#  define BOARD_GPIO_RCC_BIT  (1UL << 1)       /* AHB1ENR.GPIOBEN */
#  define BOARD_PIN_SIGNAL    12u              /* PB12           */
#  define BOARD_PIN_MARKER    13u              /* PB13           */
#  define BOARD_PIN_NAMES     "PB12 (signal), PB13 (window)"
#else
/* STM32F4 Discovery (STM32F407VG): 8 MHz HSE, 168 MHz core. PD12 and
 * PD13 are the green and orange user LEDs, also on the header. */
#  define BOARD_NAME          "STM32F4 Discovery (STM32F407VG)"
#  define BOARD_GPIO_BASE     0x40020C00UL     /* GPIOD          */
#  define BOARD_GPIO_RCC_BIT  (1UL << 3)       /* AHB1ENR.GPIODEN */
#  define BOARD_PIN_SIGNAL    12u              /* PD12           */
#  define BOARD_PIN_MARKER    13u              /* PD13           */
#  define BOARD_PIN_NAMES     "PD12 (signal), PD13 (window)"
#endif

/* Bring the core up to its full speed from the external crystal.
 * Hardware builds only (RTOS_HW); on QEMU this is an empty function,
 * because the olimex model does not emulate the PLL lock bits.
 * Returns a short description of the clock that was actually set,
 * so the boot log states what the measurements ran at. */
const char *board_clock_init(void);

#endif /* RTOS_BOARD_H */
