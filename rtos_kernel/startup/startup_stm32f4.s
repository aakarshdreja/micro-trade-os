/* ================================================================
 * startup_stm32f4.s: Bare-metal startup for STM32F4 (Cortex-M4)
 *
 * Adopted UNMODIFIED from the bare-metal boot project. It:
 *   1) Defines the 16-entry interrupt vector table at 0x08000000
 *      (already includes SVC and PendSV vectors required by the RTOS)
 *   2) Implements Reset_Handler: .data copy, .bss zero, call main()
 *
 * The vector table references SVC_Handler, PendSV_Handler and
 * SysTick_Handler by name. SVC_Handler and PendSV_Handler are defined
 * (strong) in context.s; SysTick_Handler is defined (strong) in
 * rtos.c. This file provides a .weak SysTick_Handler fallback plus
 * .weak default handlers for NMI and the fault exceptions, so the
 * image links even if a handler is left out. No edits needed here.
 * ================================================================ */

.syntax unified
.cpu cortex-m4
.fpu softvfp
.thumb

.extern _estack
.extern _sidata
.extern _sdata
.extern _edata
.extern _sbss
.extern _ebss

.global Reset_Handler

.section .isr_vector, "a", %progbits
.type g_pfnVectors, %object
.size g_pfnVectors, .-g_pfnVectors

g_pfnVectors:
    .word _estack            /* 0x00: Initial Stack Pointer          */
    .word Reset_Handler      /* 0x04: Reset vector                   */
    .word NMI_Handler        /* 0x08: Non-Maskable Interrupt         */
    .word HardFault_Handler  /* 0x0C: Unrecoverable CPU fault        */
    .word MemManage_Handler  /* 0x10: MPU violation                  */
    .word BusFault_Handler   /* 0x14: Invalid memory access          */
    .word UsageFault_Handler /* 0x18: Illegal instruction etc        */
    .word 0                  /* 0x1C: Reserved                       */
    .word 0                  /* 0x20: Reserved                       */
    .word 0                  /* 0x24: Reserved                       */
    .word 0                  /* 0x28: Reserved                       */
    .word SVC_Handler        /* 0x2C: Supervisor Call (RTOS)         */
    .word DebugMon_Handler   /* 0x30: Debug Monitor exception        */
    .word 0                  /* 0x34: Reserved                       */
    .word PendSV_Handler     /* 0x38: Pendable Service (RTOS switch) */
    .word SysTick_Handler    /* 0x3C: SysTick timer (RTOS tick)      */

.section .text.Reset_Handler, "ax", %progbits
.type Reset_Handler, %function
Reset_Handler:
    /* STEP 1: Copy .data from Flash (LMA) to RAM (VMA) */
    ldr   r0, =_sdata
    ldr   r1, =_edata
    ldr   r2, =_sidata
    movs  r3, #0
    b     .data_copy_test
.data_copy_loop:
    ldr   r4, [r2, r3]
    str   r4, [r0, r3]
    adds  r3, r3, #4
.data_copy_test:
    adds  r4, r0, r3
    cmp   r4, r1
    bcc   .data_copy_loop

    /* STEP 2: Zero-initialize .bss in RAM */
    ldr   r0, =_sbss
    ldr   r1, =_ebss
    movs  r2, #0
    b     .bss_zero_test
.bss_zero_loop:
    str   r2, [r0]
    adds  r0, r0, #4
.bss_zero_test:
    cmp   r0, r1
    bcc   .bss_zero_loop

    /* STEP 3: Call main() */
    bl    main

.hang:
    b     .hang

/* ---- Default weak exception handlers (overridden as needed) ---- */
.section .text, "ax", %progbits

.weak NMI_Handler
.type NMI_Handler, %function
NMI_Handler:
    b  NMI_Handler

.weak HardFault_Handler
.type HardFault_Handler, %function
HardFault_Handler:
    b  HardFault_Handler

.weak MemManage_Handler
.type MemManage_Handler, %function
MemManage_Handler:
    b  MemManage_Handler

.weak BusFault_Handler
.type BusFault_Handler, %function
BusFault_Handler:
    b  BusFault_Handler

.weak UsageFault_Handler
.type UsageFault_Handler, %function
UsageFault_Handler:
    b  UsageFault_Handler

.weak DebugMon_Handler
.type DebugMon_Handler, %function
DebugMon_Handler:
    b  DebugMon_Handler

.weak SysTick_Handler
.type SysTick_Handler, %function
SysTick_Handler:
    b  SysTick_Handler
