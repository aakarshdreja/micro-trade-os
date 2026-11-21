/* ================================================================
 * clock.c: Reset-and-clock bring-up for hardware builds
 *
 * Out of reset every STM32F4 runs from its 16 MHz internal RC
 * oscillator (HSI). The kernel's tick and every cycles-to-us figure
 * assume RTOS_CPU_MHZ, so on silicon the PLL must be configured
 * before anything is timed. Direct-register code, no HAL, in keeping
 * with the rest of the project.
 *
 *   Discovery  (F407): HSE 8 MHz  /8 *336 /2 = 168 MHz, 5 wait states
 *   Black Pill (F411): HSE 25 MHz /25 *200 /2 = 100 MHz, 3 wait states
 *
 * Both give a 1 MHz PLL input (the reference manual recommends 2 MHz
 * for jitter, but 1 MHz is within spec and keeps the dividers exact).
 * If the crystal does not start, the same PLL is fed from HSI so the
 * core still reaches its nominal frequency, and the boot log says so.
 * ================================================================ */

#include "board.h"

#ifndef RTOS_HW

const char *board_clock_init(void)
{
    return "not configured (QEMU model clock)";
}

#else /* RTOS_HW */

#define RCC_BASE        0x40023800UL
#define RCC_CR          (*(volatile uint32_t *)(RCC_BASE + 0x00UL))
#define RCC_PLLCFGR     (*(volatile uint32_t *)(RCC_BASE + 0x04UL))
#define RCC_CFGR        (*(volatile uint32_t *)(RCC_BASE + 0x08UL))
#define RCC_APB1ENR     (*(volatile uint32_t *)(RCC_BASE + 0x40UL))
#define PWR_CR          (*(volatile uint32_t *)0x40007000UL)
#define FLASH_ACR       (*(volatile uint32_t *)0x40023C00UL)

#define RCC_CR_HSEON    (1UL << 16)
#define RCC_CR_HSERDY   (1UL << 17)
#define RCC_CR_PLLON    (1UL << 24)
#define RCC_CR_PLLRDY   (1UL << 25)
#define RCC_APB1ENR_PWREN (1UL << 28)

#define FLASH_ACR_PRFTEN (1UL << 8)
#define FLASH_ACR_ICEN   (1UL << 9)
#define FLASH_ACR_DCEN   (1UL << 10)

#if defined(BOARD_BLACKPILL_F411)
#  define HSE_MHZ        25u
#  define PLL_N          200u
#  define PLL_Q          4u          /* 48 MHz USB clock not used      */
#  define FLASH_WS       3u          /* 90 < f <= 100 MHz at 2.7-3.6 V */
/* APB1 max 50 MHz -> /2; APB2 max 100 MHz -> /1 */
#  define CFGR_PPRE1     (4UL << 10)
#  define CFGR_PPRE2     (0UL << 13)
/* F411 VOS is two bits [15:14]; 0b11 = scale 1, needed above 84 MHz.
 * It may only be changed while the PLL is off, which it is here. */
#  define PWR_VOS_MASK   (3UL << 14)
#  define PWR_VOS_SCALE1 (3UL << 14)
#  define CLOCK_DESC_HSE "100 MHz (PLL from 25 MHz HSE crystal)"
#  define CLOCK_DESC_HSI "100 MHz (PLL from 16 MHz HSI: crystal did not start)"
#else
#  define HSE_MHZ        8u
#  define PLL_N          336u
#  define PLL_Q          7u
#  define FLASH_WS       5u          /* 150 < f <= 168 MHz at 2.7-3.6 V */
/* APB1 max 42 MHz -> /4; APB2 max 84 MHz -> /2 */
#  define CFGR_PPRE1     (5UL << 10)
#  define CFGR_PPRE2     (4UL << 13)
/* F407 VOS is one bit [14]; 1 = scale 1 (also its reset value). */
#  define PWR_VOS_MASK   (1UL << 14)
#  define PWR_VOS_SCALE1 (1UL << 14)
#  define CLOCK_DESC_HSE "168 MHz (PLL from 8 MHz HSE crystal)"
#  define CLOCK_DESC_HSI "168 MHz (PLL from 16 MHz HSI: crystal did not start)"
#endif

#define HSI_MHZ          16u
#define PLL_P_DIV2       0u          /* PLLP field 00 = divide by 2 */
#define PLLCFGR_SRC_HSE  (1UL << 22)

static void pll_config(uint32_t m, uint32_t src)
{
    RCC_PLLCFGR = (m << 0) | (PLL_N << 6) | (PLL_P_DIV2 << 16) |
                  src | (PLL_Q << 24);
}

const char *board_clock_init(void)
{
    const char *desc = CLOCK_DESC_HSE;

    /* 1. Start the crystal. Bounded wait: a missing or dead crystal
     *    must not hang the board before it can say anything. */
    RCC_CR |= RCC_CR_HSEON;
    uint32_t timeout = 200000u;
    while (((RCC_CR & RCC_CR_HSERDY) == 0u) && (--timeout != 0u)) { }

    /* 2. Regulator to scale 1 so the core may run at full speed. */
    RCC_APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC_APB1ENR;                       /* posted-write read-back */
    PWR_CR = (PWR_CR & ~PWR_VOS_MASK) | PWR_VOS_SCALE1;

    /* 3. Flash wait states BEFORE raising the clock, plus prefetch and
     *    the instruction/data caches (the ART accelerator). Without
     *    the caches every Flash fetch stalls for FLASH_WS cycles. */
    FLASH_ACR = FLASH_WS | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
    while ((FLASH_ACR & 0xFu) != FLASH_WS) { }

    /* 4. Bus prescalers: AHB /1, APB1 and APB2 within their limits. */
    RCC_CFGR = (RCC_CFGR & ~((0xFUL << 4) | (7UL << 10) | (7UL << 13))) |
               CFGR_PPRE1 | CFGR_PPRE2;

    /* 5. PLL: 1 MHz input from whichever oscillator is running. */
    if ((RCC_CR & RCC_CR_HSERDY) != 0u) {
        pll_config(HSE_MHZ, PLLCFGR_SRC_HSE);
    } else {
        RCC_CR &= ~RCC_CR_HSEON;
        pll_config(HSI_MHZ, 0u);
        desc = CLOCK_DESC_HSI;
    }
    RCC_CR |= RCC_CR_PLLON;
    while ((RCC_CR & RCC_CR_PLLRDY) == 0u) { }

    /* 6. Switch SYSCLK to the PLL and wait until the switch shows. */
    RCC_CFGR = (RCC_CFGR & ~3UL) | 2UL;
    while (((RCC_CFGR >> 2) & 3UL) != 2UL) { }

    return desc;
}

#endif /* RTOS_HW */
