/*
 * Native (SIL) FreeRTOSConfig.h for the cooperative fiber port.
 *
 * Mirrors the app-level settings of the stm32g4 board config (tick rate,
 * priorities, mutexes/semaphores, static+dynamic allocation, included API) but
 * drops the Cortex-M interrupt-priority machinery and uses the native fiber
 * port: cooperative single-thread, idle-hook quiescence handoff.
 *
 * Static allocation is enabled to match the board config so main.c's task set
 * (and the idle/timer memory-provider hooks) are target-uniform. The fiber port
 * supports it natively — static allocation reuses pxPortInitialiseStack exactly
 * as dynamic creation does, with no port-level hooks or macros of its own.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>
#include <assert.h>

#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configIDLE_SHOULD_YIELD                 1

#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        1

#define configUSE_IDLE_HOOK                     1   /* fiber-port quiescence handoff */
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            0
#define configCHECK_FOR_STACK_OVERFLOW          0

#define configTICK_RATE_HZ                      ( ( TickType_t ) 1000 )
#define configMAX_PRIORITIES                    ( 56 )
#define configMINIMAL_STACK_SIZE                ( ( uint16_t ) 128 )
#define configMAX_TASK_NAME_LEN                 ( 16 )
#define configTOTAL_HEAP_SIZE                   ( ( size_t ) ( 128 * 1024 ) )
#define configUSE_16_BIT_TICKS                  0

#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_QUEUE_SETS                    0
#define configUSE_TASK_NOTIFICATIONS            1
#define configQUEUE_REGISTRY_SIZE               8

#define configUSE_TIMERS                        0   /* enable when the sim needs sw timers */
#define configUSE_CO_ROUTINES                   0
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_TICKLESS_IDLE                 0
#define configUSE_TRACE_FACILITY                0

#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_xTaskGetTickCount               1
#define INCLUDE_eTaskGetState                   1

#define configASSERT( x )                       assert( x )

#endif /* FREERTOS_CONFIG_H */
