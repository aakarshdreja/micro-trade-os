/* ================================================================
 * startup_stm32f4.s: Bare-metal startup for STM32F4 (Cortex-M4)
 *
 * PURPOSE: This file is the very first code that runs after reset.
 * It has two responsibilities:
 *   1) Define the interrupt vector table at address 0x08000000
 *   2) Implement Reset_Handler. Initialize runtime, call main()
 *
 * No C runtime (crt0), no vendor HAL, everything from scratch.
 * ================================================================ */
 
/* ---- Assembler directives ----
 * .syntax unified : Use unified ARM/Thumb syntax (required for Thumb2)
 * .cpu cortex-m4  : Tell assembler we target a Cortex-M4 core
 * .fpu softvfp    : Software floating-point ABI (QEMU default)
 * .thumb          : Emit Thumb2 instructions (Cortex-M4 only runs Thumb) */
.syntax unified
.cpu cortex-m4
.fpu softvfp
.thumb
 
/* ---- Symbols from the linker script ----------------------------
 * These are defined in ld/linker.ld and resolved at link time.
 * .extern is optional but documents that these come from outside. */
.extern _estack     /* Top of stack = end of RAM = 0x20020000 */
.extern _sidata     /* Flash address where .data image starts  */
.extern _sdata      /* RAM address where .data section starts  */
.extern _edata      /* RAM address where .data section ends    */
.extern _sbss       /* RAM address where .bss section starts   */
.extern _ebss       /* RAM address where .bss section ends     */
 
/* ---- Make Reset_Handler visible to the linker ---- */
.global Reset_Handler
 
/* ================================================================
 * SECTION 1: INTERRUPT VECTOR TABLE
 *
 * Must be placed at the very start of Flash (0x08000000).
 * The linker script places .isr_vector first because of:
 *   .isr_vector : { *(.isr_vector) } > FLASH
 *
 * "a" = allocatable (takes up space in the binary)
 * %progbits = contains program data (not uninitialized storage)
 * KEEP() in linker script prevents --gc-sections from discarding
 * this section even though nothing explicitly calls the table.
 * ================================================================ */
.section .isr_vector, "a", %progbits
.type g_pfnVectors, %object   /* Declare as data object, not function */
.size g_pfnVectors, .-g_pfnVectors
 
g_pfnVectors:
    /* Offset 0x00, Initial Stack Pointer.
     * _estack = ORIGIN(RAM) + LENGTH(RAM) = 0x20000000 + 0x20000 = 0x20020000
     * The stack grows downward, so we start at the top of RAM. */
    .word _estack
 
    /* Offset 0x04, Reset Vector.
     * The address of Reset_Handler. The linker will set LSB=1 automatically
     * because Reset_Handler is a Thumb function (.thumb directive above).
     * GDB will show this as 0x08000149 (or similar), that is correct. */
    .word Reset_Handler
 
    /* Offsets 0x08-0x3C, Exception vectors.
     * These must exist even if you don't use them, because the
     * CPU might generate these exceptions for any bug in your code.
     * Default handlers (defined below) loop forever so GDB can catch them. */
    .word NMI_Handler        /* 0x08: Non-Maskable Interrupt  */
    .word HardFault_Handler  /* 0x0C: Unrecoverable CPU fault */
    .word MemManage_Handler  /* 0x10: MPU violation           */
    .word BusFault_Handler   /* 0x14: Invalid memory access   */
    .word UsageFault_Handler /* 0x18: Illegal instruction etc */
    .word 0                  /* 0x1C: Reserved                */
    .word 0                  /* 0x20: Reserved                */
    .word 0                  /* 0x24: Reserved                */
    .word 0                  /* 0x28: Reserved                */
    .word SVC_Handler        /* 0x2C: Supervisor Call (SVC)   */
    .word DebugMon_Handler   /* 0x30: Debug Monitor exception */
    .word 0                  /* 0x34: Reserved                */
    .word PendSV_Handler     /* 0x38: Pendable Service Call   */
    .word SysTick_Handler    /* 0x3C: SysTick timer overflow  */
 
/* ================================================================
 * SECTION 2: RESET HANDLER
 *
 * This is the entry point. It runs immediately after reset.
 * The .text.Reset_Handler section is placed inside .text in Flash
 * by the linker script: .text : { *(.text*) } > FLASH
 *
 * "ax" = allocatable + executable
 * ================================================================ */
.section .text.Reset_Handler, "ax", %progbits
.type Reset_Handler, %function   /* Tell linker/debugger this is code */
Reset_Handler:
 
    /* ============================================================
     * STEP 1: Copy .data from Flash to RAM
     *
     * Why: Global variables with initial values (like int x = 5;)
     * are stored in Flash at link time. But they must live in RAM
     * at runtime (because Flash is read-only). So we must copy
     * them before main() runs.
     *
     * _sidata = Load Memory Address (LMA): Flash source of .data
     * _sdata  = Virtual Memory Address (VMA): RAM destination start
     * _edata  = RAM destination end
     *
     * Algorithm: copy word-by-word from [_sidata] to [_sdata.._edata]
     * ============================================================ */
    ldr   r0, =_sdata      /* r0 = destination start pointer (RAM)   */
    ldr   r1, =_edata      /* r1 = destination end pointer (RAM)     */
    ldr   r2, =_sidata     /* r2 = source start pointer (Flash)      */
    movs  r3, #0           /* r3 = byte offset, starts at 0          */
    b     .data_copy_test  /* Branch to loop condition first         */
 
.data_copy_loop:
    ldr   r4, [r2, r3]     /* Load 4 bytes from Flash at (r2 + r3)   */
    str   r4, [r0, r3]     /* Store 4 bytes to RAM at (r0 + r3)      */
    adds  r3, r3, #4       /* Advance offset by 4 bytes (one word)   */
 
.data_copy_test:
    adds  r4, r0, r3       /* r4 = current destination address        */
    cmp   r4, r1           /* Compare to _edata                       */
    bcc   .data_copy_loop  /* Branch if r4 < r1 (copy not done yet)  */
 
    /* ============================================================
     * STEP 2: Zero-initialize .bss in RAM
     *
     * Why: The C standard requires that global/static variables with
     * no explicit initializer are set to zero before main() runs.
     * Example: int uninitialized; must equal 0 in main().
     * These live in the .bss section. They have no Flash image
     * (no data to copy), we just zero them in RAM.
     *
     * _sbss = RAM start of .bss section
     * _ebss = RAM end of .bss section
     * ============================================================ */
    ldr   r0, =_sbss       /* r0 = current address pointer           */
    ldr   r1, =_ebss       /* r1 = end address                       */
    movs  r2, #0           /* r2 = the zero value we will store       */
    b     .bss_zero_test   /* Branch to loop condition first         */
 
.bss_zero_loop:
    str   r2, [r0]         /* Write 0 to address in r0               */
    adds  r0, r0, #4       /* Advance pointer by 4 bytes             */
 
.bss_zero_test:
    cmp   r0, r1           /* Compare current pointer to _ebss       */
    bcc   .bss_zero_loop   /* Branch if r0 < r1 (not done yet)       */
 
    /* ============================================================
     * STEP 3: Call main()
     *
     * bl (Branch with Link) calls main() and stores the return
     * address in LR so that if main() ever returns, we catch it.
     * ============================================================ */
    bl    main
 
    /* ============================================================
     * STEP 4: Hang if main() returns
     *
     * A well-written embedded main() never returns. But if it does,
     * we must not let the CPU run off into random memory. This
     * infinite loop catches that case. Visible in GDB as "hang".
     * ============================================================ */
.hang:
    b     .hang
 
/* ================================================================
 * SECTION 3: DEFAULT EXCEPTION HANDLERS
 *
 * These are "weak" symbols. That means:
 * - If you define your own SysTick_Handler in main.c, the linker
 *   uses YOUR version and ignores this one.
 * - If you don't define it, this fallback is used.
 *
 * Each handler loops forever ("b ."). This is intentional:
 * - It prevents the CPU from running undefined code.
 * - GDB will show you exactly which handler fired.
 * - You can set a breakpoint here to catch faults.
 * ================================================================ */
.section .text, "ax", %progbits
 
/* NMI: Non-Maskable Interrupt. Cannot be disabled */
.weak NMI_Handler
.type NMI_Handler, %function
NMI_Handler:
    b  NMI_Handler         /* Infinite loop. Trap here for debugging */
 
/* HardFault: catches undefined instructions, bad memory access, etc. */
.weak HardFault_Handler
.type HardFault_Handler, %function
HardFault_Handler:
    b  HardFault_Handler
 
/* MemManage: MPU (Memory Protection Unit) violation */
.weak MemManage_Handler
.type MemManage_Handler, %function
MemManage_Handler:
    b  MemManage_Handler
 
/* BusFault: attempt to access non-existent peripheral or invalid address */
.weak BusFault_Handler
.type BusFault_Handler, %function
BusFault_Handler:
    b  BusFault_Handler
 
/* UsageFault: illegal instruction, divide by zero, unaligned access */
.weak UsageFault_Handler
.type UsageFault_Handler, %function
UsageFault_Handler:
    b  UsageFault_Handler
 
/* SVCall: software-triggered exception via SVC instruction */
.weak SVC_Handler
.type SVC_Handler, %function
SVC_Handler:
    b  SVC_Handler
 
/* DebugMon: debug monitor exception */
.weak DebugMon_Handler
.type DebugMon_Handler, %function
DebugMon_Handler:
    b  DebugMon_Handler
 
/* PendSV: pendable service call, often used by RTOS context switches */
.weak PendSV_Handler
.type PendSV_Handler, %function
PendSV_Handler:
    b  PendSV_Handler
 
/* SysTick: timer overflow. Weak here, overridden by main.c definition */
.weak SysTick_Handler
.type SysTick_Handler, %function
SysTick_Handler:
    b  SysTick_Handler
