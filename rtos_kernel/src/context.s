/* ================================================================
 * context.s: ARM Cortex-M4 context switch primitives
 *
 * Implements the two exception handlers the RTOS needs:
 *   SVC_Handler. Launches the very first task
 *   PendSV_Handler. Performs the task-to-task context switch
 *
 * Convention (canonical ARM RTOS pattern):
 *   - Handlers run on the Main Stack Pointer (MSP)
 *   - Tasks run on the Process Stack Pointer (PSP)
 *   - Hardware auto-saves r0-r3,r12,lr,pc,xpsr on exception entry
 *   - Software here saves/restores the callee-saved r4-r11
 *
 * Globals (defined in rtos.c):
 *   rtos_current_task -> tcb_t* whose FIRST member is 'sp'
 *   rtos_next_task    -> tcb_t* selected by the scheduler
 * ================================================================ */

.syntax unified
.cpu cortex-m4
.fpu softvfp
.thumb

.global SVC_Handler
.global PendSV_Handler
.global rtos_start_first_task

/* ================================================================
 * rtos_start_first_task. Kicks the scheduler by issuing an SVC.
 * Called once from rtos_start() (C). Triggers SVC_Handler below.
 * ================================================================ */
.type rtos_start_first_task, %function
rtos_start_first_task:
    svc   #0
    bx    lr          /* never reached; SVC_Handler returns to a task */

/* ================================================================
 * SVC_Handler. Start the first task.
 *
 * rtos_next_task->sp points to a fully-initialised stack frame:
 *     [ r4-r11 ][ r0-r3, r12, lr, pc(entry), xpsr ]
 * We pop the software-saved r4-r11, load PSP at the hardware frame,
 * switch the CPU to use PSP in Thread mode, then exception-return
 * straight into the task entry point.
 * ================================================================ */
.type SVC_Handler, %function
SVC_Handler:
    ldr   r3, =rtos_next_task
    ldr   r2, [r3]          /* r2 = rtos_next_task            */
    ldr   r0, [r2]          /* r0 = next->sp (first member)   */

    ldmia r0!, {r4-r11}     /* restore callee-saved registers */
    msr   psp, r0           /* PSP -> hardware exception frame */

    ldr   r3, =rtos_current_task
    str   r2, [r3]          /* rtos_current_task = rtos_next_task */

    movs  r0, #2
    msr   control, r0       /* use PSP, Thread mode, privileged */
    isb

    ldr   lr, =0xFFFFFFFD    /* EXC_RETURN: Thread mode, PSP     */
    bx    lr

/* ================================================================
 * PendSV_Handler, the context switch.
 *
 * Runs at the lowest exception priority, so it executes only after
 * all other ISRs have finished (no nested-switch races). It saves
 * the outgoing task's r4-r11 + PSP into its TCB, swaps in the
 * incoming task's TCB, restores its r4-r11 + PSP, and returns.
 * ================================================================ */
.type PendSV_Handler, %function
PendSV_Handler:
    cpsid i                 /* atomic wrt other interrupts      */

    /* --- timestamp the start of the switch ---------------------
     * Reserve an 8-byte, 8-aligned frame on MSP: [t0][EXC_RETURN].
     * rtos_cycles() and the recorder are AAPCS calls that preserve
     * r4-r11, so the task's callee-saved registers survive them. */
    sub   sp, sp, #8
    str   lr, [sp, #4]      /* save EXC_RETURN                  */
    bl    rtos_cycles       /* r0 = t0 (start cycles)           */
    str   r0, [sp, #0]

    /* --- save outgoing context -------------------------------- */
    mrs   r0, psp           /* r0 = current task's PSP          */
    ldr   r3, =rtos_current_task
    ldr   r2, [r3]          /* r2 = rtos_current_task           */
    cbz   r2, PendSV_no_save/* first switch: nothing to save    */

    stmdb r0!, {r4-r11}     /* push r4-r11 onto the task stack  */
    str   r0, [r2]          /* current->sp = updated PSP        */

PendSV_no_save:
    /* --- select and restore incoming context ------------------ */
    ldr   r1, =rtos_next_task
    ldr   r2, [r1]          /* r2 = rtos_next_task              */
    str   r2, [r3]          /* rtos_current_task = rtos_next_task */

    ldr   r0, [r2]          /* r0 = next->sp                    */
    ldmia r0!, {r4-r11}     /* pop r4-r11                       */
    msr   psp, r0           /* PSP -> next task's hw frame      */
    isb

    /* --- reprogram the MPU stack guard for the incoming task ---
     * No-op unless the kernel was built with -DRTOS_ENABLE_MPU.
     * Part of the switch, so it is included in the measured cost. */
    bl    rtos_mpu_on_switch

    /* --- timestamp the end and record the switch cost --------- */
    bl    rtos_cycles       /* r0 = t1 (end cycles)             */
    ldr   r1, [sp, #0]      /* r1 = t0                          */
    subs  r0, r0, r1        /* r0 = delta cycles                */
    bl    rtos_stats_record_switch

    ldr   lr, [sp, #4]      /* restore EXC_RETURN               */
    add   sp, sp, #8

    cpsie i
    bx    lr
