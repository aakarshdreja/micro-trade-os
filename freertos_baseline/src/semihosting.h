/* semihosting.h: ARM Semihosting interface for QEMU output
 * Semihosting lets embedded firmware print to the host terminal
 * by triggering a BKPT 0xAB software breakpoint. QEMU intercepts
 * this breakpoint and services the request instead of stopping. */

#pragma once

#include <stdint.h>

/* SYS_WRITE0 (0x04): Write a null-terminated string to the host console */
#define SEMIHOSTING_SYS_WRITE0  0x04

/* Register-pinned form: binds reason->r0 and arg->r1 explicitly so
 * the compiler cannot alias the inputs into the clobbered registers.
 * The naive "mov r0,%1 / mov r1,%2" pattern is miscompiled at -O2
 * (the input operands may already live in r0/r1). */
static inline int semihosting_call(int reason, void *arg)
{
    register int   r0 __asm("r0") = reason;
    register void *r1 __asm("r1") = arg;
    __asm volatile (
        "bkpt 0xAB\n"
        : "+r"(r0)
        : "r"(r1)
        : "memory"
    );
    return r0;
}

/* sh_puts(). Print a null-terminated string to the host terminal */
static inline void sh_puts(const char *s)
{
#ifdef RTOS_HW
    /* On silicon, BKPT with no debugger attached escalates to a
     * HardFault and the board stops dead. DHCSR.C_DEBUGEN (bit 0) is
     * set only while a debugger (OpenOCD via the ST-Link) holds the
     * core, so check it and stay silent when running standalone. */
    if (((*(volatile uint32_t *)0xE000EDF0UL) & 1u) == 0u) {
        return;
    }
#endif
    semihosting_call(SEMIHOSTING_SYS_WRITE0, (void *)s);
}
