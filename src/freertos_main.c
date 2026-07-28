/**
 * @file    FreeRTOS entry for the LVGL PC simulator
 *
 * Two tasks:
 *   - UI task         — owns LVGL: lv_init() + sdl_hal_init() + ui_init(),
 *                       then loops drain-queue + lv_timer_handler.
 *                       LVGL is NOT thread-safe; only this task touches it.
 *   - Dashboard producer — pushes wandering System/Panels snapshots via
 *                       ui_set_system/ui_set_panels.
 */

#ifdef USE_FREERTOS

#include "FreeRTOS.h"
#include "task.h"

#include "lvgl/lvgl.h"
#include "hal/hal.h"
#include "ui.h"
#include "ui_state.h"
#include "demo_source.h"

#ifdef PRODUCER_SOCKET
extern void socket_transport_task(void *pv);
#endif

#include <stdio.h>
#include <stdlib.h>

#define UI_TASK_STACK_WORDS         8192
#define UI_TASK_PRIORITY            ( tskIDLE_PRIORITY + 2 )

#define DEMO_TASK_STACK_WORDS       1024
#define DEMO_TASK_PRIORITY          ( tskIDLE_PRIORITY + 1 )

#define UI_FALLBACK_TICK_MS         5
#define DEMO_PERIOD_MS              500

static void ui_task(void * pvParameters)
{
    (void)pvParameters;

    lv_init();
    sdl_hal_init(1024, 600);  // match the H757 production panel
    ui_init();

    for(;;) {
        uint32_t sleep_ms = ui_runtime_tick();
        if(sleep_ms == 0) {
            sleep_ms = UI_FALLBACK_TICK_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

#ifndef PRODUCER_SOCKET
/* Demo producer — drives every UI domain from the portable src/demo/ module.
 * The fabrication logic lives there so the sim and both firmware targets share
 * one copy; this task is just the platform's timing loop. */
static void dashboard_producer_task(void * pvParameters)
{
    (void)pvParameters;
    for(;;) {
        demo_source_tick(250);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
#endif /* PRODUCER_SOCKET */

void vApplicationMallocFailedHook(void)
{
    printf("FreeRTOS: malloc failed (free heap=%lu)\n",
           (unsigned long)xPortGetFreeHeapSize());
    for(;;);
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char * pcTaskName)
{
    (void)xTask;
    printf("FreeRTOS: stack overflow in task %s\n", pcTaskName);
    for(;;);
}

void vAssertCalled(unsigned long ulLine, const char * const pcFileName)
{
    printf("FreeRTOS assert: %s:%lu\n", pcFileName, ulLine);
    fflush(stdout);
    for(;;);
}

int main(int argc, char ** argv)
{
    (void)argc;
    (void)argv;

    ui_runtime_init();

    if(xTaskCreate(ui_task, "UI", UI_TASK_STACK_WORDS,
                   NULL, UI_TASK_PRIORITY, NULL) != pdPASS) {
        printf("Failed to create UI task\n");
        return 1;
    }

#ifdef PRODUCER_SOCKET
    if(xTaskCreate(socket_transport_task, "Sock", 2048,
                   NULL, DEMO_TASK_PRIORITY, NULL) != pdPASS) {
        printf("Failed to create socket transport task\n");
        return 1;
    }
#else
    if(xTaskCreate(dashboard_producer_task, "Dash", DEMO_TASK_STACK_WORDS,
                   NULL, DEMO_TASK_PRIORITY, NULL) != pdPASS) {
        printf("Failed to create dashboard producer task\n");
        return 1;
    }
#endif

    vTaskStartScheduler();

    /* Should never reach here. */
    printf("Scheduler returned\n");
    return 0;
}

#endif /* USE_FREERTOS */
