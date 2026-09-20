/* ================================================================
 * main.c: Bare-metal application for STM32F4
 *
 * This file implements:
 *   Part B: Runtime initialization verification
 *   Part D: Observable output via ARM semihosting
 *   Part E: SysTick timer configuration and interrupt handler
 *
 * Called by: Reset_Handler in startup_stm32f4.s
 * Requires:  .data and .bss initialized before entry
 * ================================================================ */
 
#include <stdint.h>       /* For uint32_t, int32_t etc. */
#include "semihosting.h"  /* For sh_puts(), semihosting output */
 
/* ================================================================
 * PART B, RUNTIME INITIALIZATION VERIFICATION VARIABLES
 *
 * These two variables test that Reset_Handler correctly initializes
 * memory before main() is called.
 *
 * 'initialized' lives in .data section:
 *   - Compiler stores value 123 (0x7B) in Flash at link time
 *   - Reset_Handler copies it from Flash to RAM
 *   - If copy worked: initialized == 123 when main() runs
 *
 * 'uninitialized' lives in .bss section:
 *   - No initial value specified → C standard says it must be 0
 *   - Reset_Handler zeroes the .bss region in RAM
 *   - If zeroing worked: uninitialized == 0 when main() runs
 * ================================================================ */
int initialized   = 123;  /* .data: stored in Flash, copied to RAM   */
int uninitialized;         /* .bss: no Flash copy, zeroed in RAM      */
 
/* ================================================================
 * PART E, SYSTICK COUNTER
 *
 * This variable is incremented inside SysTick_Handler each time
 * the SysTick timer overflows (approximately every millisecond).
 *
 * 'volatile' is critical here:
 *   Without it, the compiler might cache the value in a register
 *   and never re-read from memory. Since the ISR modifies it
 *   asynchronously (from the CPU's point of view), the compiler
 *   must always read/write to actual memory, not a register copy.
 * ================================================================ */
volatile uint32_t systick_count = 0;
 
/* ================================================================
 * SYSTICK REGISTER DEFINITIONS
 *
 * The SysTick timer is part of the Cortex-M4 core, it exists on
 * every Cortex-M chip regardless of vendor. Registers are at fixed
 * addresses in the System Control Block (Private Peripheral Bus).
 *
 * Base address: 0xE000E010
 *   +0x00: CTRL, Control and Status register
 *   +0x04: LOAD, Reload Value register
 *   +0x08: VAL, Current Value register
 *   +0x0C: CALIB, Calibration register (not used here)
 *
 * We define these as volatile pointer dereferences so every access
 * generates an actual memory read/write (no compiler caching).
 * ================================================================ */
#define SYSTICK_BASE     0xE000E010UL
#define SYSTICK_CTRL   (*(volatile uint32_t *)(SYSTICK_BASE + 0x00U))
#define SYSTICK_LOAD   (*(volatile uint32_t *)(SYSTICK_BASE + 0x04U))
#define SYSTICK_VAL    (*(volatile uint32_t *)(SYSTICK_BASE + 0x08U))
 
/* Bit definitions for SYSTICK_CTRL register:
 * Bit 0 (ENABLE)    : 1 = counter enabled, 0 = disabled
 * Bit 1 (TICKINT)   : 1 = generate interrupt on underflow
 * Bit 2 (CLKSOURCE) : 1 = use processor clock, 0 = external ref clock */
#define SYSTICK_CTRL_ENABLE     (1UL << 0)
#define SYSTICK_CTRL_TICKINT    (1UL << 1)
#define SYSTICK_CTRL_CLKSOURCE  (1UL << 2)
 
/* Reload value determines interrupt frequency:
 *   Frequency = CPU_clock / (RELOAD + 1)
 * QEMU emulates ~8 MHz for this machine model.
 * RELOAD = 7999 → interrupt every 8000 cycles → ~1000 Hz (1ms period)
 * Maximum allowed RELOAD value is 0xFFFFFF (24-bit register). */
#define SYSTICK_RELOAD_VALUE    7999UL
 
/* ================================================================
 * systick_init(), Configure and start the SysTick timer
 *
 * Call this once from main() after printing the boot message.
 * After this returns, SysTick_Handler fires ~1000 times per second.
 * ================================================================ */
static void systick_init(void)
{
    /* Step 1: Disable SysTick before reconfiguring.
     * Writing 0 to CTRL stops the counter and disables the interrupt.
     * This prevents a spurious interrupt during setup. */
    SYSTICK_CTRL = 0;
 
    /* Step 2: Set the reload (auto-reset) value.
     * The counter counts DOWN from LOAD to 0. When it reaches 0,
     * it triggers the interrupt (if TICKINT is set), then reloads
     * from this register and counts down again. */
    SYSTICK_LOAD = SYSTICK_RELOAD_VALUE;
 
    /* Step 3: Clear the current value register.
     * Writing any value to VAL clears it to 0. This ensures the
     * first interrupt fires after exactly RELOAD+1 cycles, not some
     * arbitrary leftover value. */
    SYSTICK_VAL = 0;
 
    /* Step 4: Enable SysTick with processor clock and interrupt.
     * Set all three bits simultaneously in one write:
     *   CLKSOURCE = 1 → use CPU clock (most reliable in QEMU)
     *   TICKINT   = 1 → trigger SysTick_Handler on underflow
     *   ENABLE    = 1 → start the counter */
    SYSTICK_CTRL = SYSTICK_CTRL_CLKSOURCE |
                   SYSTICK_CTRL_TICKINT   |
                   SYSTICK_CTRL_ENABLE;
}
 
/* ================================================================
 * SysTick_Handler, Interrupt Service Routine for SysTick timer
 *
 * This function overrides the weak default in startup_stm32f4.s.
 * The name MUST match exactly: "SysTick_Handler"
 * The linker resolves the vector table entry to this function.
 *
 * Called by: Hardware, approximately every 1ms (1000 Hz)
 * Context:   Interrupt context. Keep it short and fast
 *
 * IMPORTANT: In production embedded code, you would NEVER call
 * semihosting from an ISR. However, in QEMU this works fine for
 * debugging purposes and is explicitly allowed for this assignment.
 * ================================================================ */
void SysTick_Handler(void)
{
    /* Increment the tick counter.
     * 'volatile' on systick_count ensures this write goes to memory,
     * not just a register. Main() can observe the change. */
    systick_count++;
 
    /* Print a message every 100 ticks to show the ISR is alive.
     * The modulo check produces output about 10 times per second.
     * This provides observable QEMU output without flooding the terminal. */
    if (systick_count % 100 == 0)
    {
        sh_puts("SysTick: 100 ticks elapsed\r\n");
    }
}
 
/* ================================================================
 * main(), C application entry point
 *
 * Called by: Reset_Handler (after .data copy and .bss zeroing)
 * Pre-conditions:
 *   - Stack pointer is valid (set by hardware from vector table)
 *   - .data section copied from Flash to RAM by Reset_Handler
 *   - .bss section zeroed in RAM by Reset_Handler
 * ================================================================ */
int main(void)
{
    /* ---- Part D: Prove we reached main() ----
     * If this prints, the vector table, Reset_Handler, and
     * semihosting infrastructure are all working correctly. */
    sh_puts("=== STM32F4 Bare-Metal Boot OK ===\r\n");
 
    /* ---- Part B: Verify .data initialization ----
     * 'initialized' was set to 123 at compile time.
     * If Reset_Handler correctly copied .data from Flash to RAM,
     * this variable will equal 123 right now. */
    if (initialized == 123)
    {
        sh_puts("PASS: initialized == 123 (.data copy worked)\r\n");
    }
    else
    {
        sh_puts("FAIL: initialized != 123 (.data copy broken!)\r\n");
    }
 
    /* ---- Part B: Verify .bss zero-initialization ----
     * 'uninitialized' has no initializer.
     * If Reset_Handler correctly zeroed .bss, this must equal 0. */
    if (uninitialized == 0)
    {
        sh_puts("PASS: uninitialized == 0 (.bss zeroing worked)\r\n");
    }
    else
    {
        sh_puts("FAIL: uninitialized != 0 (.bss zeroing broken!)\r\n");
    }
 
    /* ---- Part E: Configure and start SysTick timer ---- */
    systick_init();
    sh_puts("SysTick initialized. Interrupts firing at ~1kHz\r\n");
 
    /* ---- Infinite loop: yield to SysTick interrupt handler ----
     * main() must never return (there is nothing to return to).
     * The while(1) keeps the program alive; SysTick_Handler
     * fires periodically in the background. */
    while (1)
    {
        /* Nothing to do here, SysTick_Handler handles periodic tasks.
         * In a real application, you might use WFI (Wait For Interrupt)
         * to save power: __asm("wfi"); */
    }
 
    return 0;
}
