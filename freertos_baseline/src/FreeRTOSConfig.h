/* ================================================================
 * FreeRTOSConfig.h. The BASELINE kernel's configuration
 *
 * Every setting here exists to make the comparison against our
 * kernel fair. Where FreeRTOS offers a choice, the option chosen is
 * the one that matches what our kernel does, so that the measured
 * difference is attributable to the kernel design and not to a
 * configuration mismatch:
 *
 *   - 8 MHz core clock, identical to the QEMU olimex-stm32-h405 model
 *   - 1 kHz tick, identical to our SysTick reload
 *   - PREEMPTIVE scheduling with time-slicing, identical to our
 *     fixed-priority round-robin-within-a-band scheduler
 *   - 8 priority levels, identical to RTOS_MAX_PRIORITIES
 *   - Static allocation available, dynamic used for queues (which is
 *     what a normal FreeRTOS application does)
 *
 * Trace/stats hooks are DISABLED except the one switch counter the
 * benchmark needs, because runtime stats gathering would tax the
 * baseline for something our kernel is not doing either.
 *
 * This is deliberately NOT a crippled configuration. FreeRTOS is the
 * thing we claim to beat; beating a hobbled version of it would
 * prove nothing.
 * ================================================================ */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1   /* round-robin within a priority */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1   /* CLZ-based, as ours is */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0

#define configCPU_CLOCK_HZ                      ( 8000000UL )   /* QEMU model */
#define configTICK_RATE_HZ                      ( 1000 )        /* 1 kHz, as ours */
#define configMAX_PRIORITIES                    ( 8 )           /* as RTOS_MAX_PRIORITIES */
#define configMINIMAL_STACK_SIZE                ( 128 )
#define configTOTAL_HEAP_SIZE                   ( 24 * 1024 )
#define configMAX_TASK_NAME_LEN                 ( 12 )
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_QUEUE_SETS                    0
#define configUSE_TASK_NOTIFICATIONS            1
#define configSUPPORT_STATIC_ALLOCATION         0   /* queues/tasks created dynamically */
#define configSUPPORT_DYNAMIC_ALLOCATION        1

/* Our kernel bounds priority inversion with a priority-inheritance
 * mutex; FreeRTOS mutexes inherit too, so this is enabled to keep
 * the two comparable. */
#define configUSE_MUTEXES                       1

/* Hooks and checks off: they would add work our kernel is not doing. */
#define configCHECK_FOR_STACK_OVERFLOW          0
#define configUSE_MALLOC_FAILED_HOOK            0
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* Software timers and co-routines unused by the benchmarks. */
#define configUSE_TIMERS                        0
#define configUSE_CO_ROUTINES                   0

/* API functions the benchmark harness needs. */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_uxTaskGetStackHighWaterMark     0

/* ---- Interrupt priorities (Cortex-M) --------------------------
 * The kernel's own handlers must be the LOWEST priority, exactly as
 * our kernel programs SHPR3. */
#define configKERNEL_INTERRUPT_PRIORITY         255
/* The STM32F4 implements the top 4 bits of the 8-bit priority field,
 * so the low nibble must be zero or the port's own configASSERT
 * rejects the value. Priority 5 therefore encodes as 5 << 4 = 0x50. */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    ( 5 << 4 )   /* 0x50 */

/* ---- Handler name mapping -------------------------------------
 * Our vector table (startup_stm32f4.s, shared verbatim with the RTOS
 * build) names the CMSIS handlers. FreeRTOS's port defines its own
 * names, so they are aliased here rather than editing the shared
 * startup file. Keeping the boot layer byte-identical between the
 * two images is part of what makes the comparison fair. */
#define vPortSVCHandler                         SVC_Handler
#define xPortPendSVHandler                      PendSV_Handler
#define xPortSysTickHandler                     SysTick_Handler

/* ---- Context-switch instrumentation ---------------------------
 * Our kernel times every switch inside PendSV. To measure the same
 * quantity on FreeRTOS we use the trace hooks, which are the
 * supported way to do this and cost one function call per switch on
 * each side. Defined in bench_common.c. */
#ifdef FRTOS_BENCH_CTXSW
    void bench_trace_switched_out(void);
    void bench_trace_switched_in(void);
    #define traceTASK_SWITCHED_OUT()  bench_trace_switched_out()
    #define traceTASK_SWITCHED_IN()   bench_trace_switched_in()
#endif

/* A silent spin on a failed assertion is indistinguishable from a
 * hang, so the assertion reports where it fired before stopping. */
void bench_assert_failed(const char *file, int line);
#define configASSERT( x )  if( ( x ) == 0 ) { bench_assert_failed( __FILE__, __LINE__ ); }

#endif /* FREERTOS_CONFIG_H */
