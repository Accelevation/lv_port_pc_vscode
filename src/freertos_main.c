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

/* Dashboard producer — fabricates wandering System + Panels snapshots so the
 * live-data path is exercised end to end. This is a stand-in for a real CAN/
 * RS485/Modbus producer; the UI is identical either way. */
static void dashboard_producer_task(void * pvParameters)
{
    (void)pvParameters;
    int tick = 0;
    for(;;) {
        float phase = (float)tick * 0.10f;

        ui_system_t sys;
        sys.node_state     = UI_NODE_ONLINE;
        sys.nodes_synced   = 1;
        /* wander around the seed values with a cheap integer-driven ripple */
        sys.global_load_kw = 82.4f + (float)((tick * 7) % 40) / 10.0f - 2.0f;   /* ~80.4–86.4 */
        sys.efficiency_pct = 97.5f + (float)((tick * 3) % 20) / 10.0f;          /* ~97.5–99.5 */
        (void)phase;
        ui_set_system(&sys);

        /* Panels update more slowly (every ~1 s) to show store persistence on
         * navigation-return, not just live refresh. */
        if((tick % 4) == 0) {
            ui_panel_t panels[2];
            snprintf(panels[0].id, sizeof panels[0].id, "PANEL ALPHA-1");
            panels[0].is_master = 1;
            panels[0].healthy   = 1;
            panels[0].load_amps = 240 + (tick % 20);
            panels[0].voltage_v = 482.1f;
            snprintf(panels[1].id, sizeof panels[1].id, "PANEL BRAVO-2");
            panels[1].is_master = 0;
            panels[1].healthy   = ((tick / 4) % 6 != 0);  /* occasionally trips unhealthy */
            panels[1].load_amps = 305 + (tick % 15);
            panels[1].voltage_v = 479.4f;
            ui_set_panels(panels, 2);
        }

        ++tick;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

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

    if(xTaskCreate(dashboard_producer_task, "Dash", DEMO_TASK_STACK_WORDS,
                   NULL, DEMO_TASK_PRIORITY, NULL) != pdPASS) {
        printf("Failed to create dashboard producer task\n");
        return 1;
    }

    vTaskStartScheduler();

    /* Should never reach here. */
    printf("Scheduler returned\n");
    return 0;
}

#endif /* USE_FREERTOS */
