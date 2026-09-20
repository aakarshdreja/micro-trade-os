/* semihosting.h: ARM Semihosting interface for QEMU output
 * Semihosting lets embedded firmware print to the host terminal
 * by triggering a BKPT 0xAB software breakpoint. QEMU intercepts
 * this breakpoint and services the request instead of stopping. */
 
#pragma once
 
/* SYS_WRITE0 (0x04): Write a null-terminated string to the host console */
#define SEMIHOSTING_SYS_WRITE0  0x04
 
/* semihosting_call(). Invokes a semihosting operation
 * reason : operation code (e.g., SEMIHOSTING_SYS_WRITE0)
 * arg    : pointer to argument block or string
 * Returns: result code from the host (0 = success for WRITE0)    */
static inline int semihosting_call(int reason, void *arg)
{
    int value;
    __asm volatile (
        "mov r0, %1\n"   /* Load operation code into r0            */
        "mov r1, %2\n"   /* Load argument pointer into r1          */
        "bkpt 0xAB\n"   /* BKPT 0xAB = semihosting trap to host   */
        "mov %0, r0\n"   /* Capture return value from r0           */
        : "=r"(value)
        : "r"(reason), "r"(arg)
        : "r0", "r1", "memory"
    );
    return value;
}
 
/* sh_puts(). Print a null-terminated string to the host terminal */
static inline void sh_puts(const char *s)
{
    semihosting_call(SEMIHOSTING_SYS_WRITE0, (void*)s);
}
