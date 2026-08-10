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

#include "FreeRTOS.h"
#include "task.h"

#include "lvgl/lvgl.h"
#include "hal/hal.h"
#include "ui.h"
#include "ui_state.h"
#include "demo_source.h"
#include "settings.h"
#include "transport.h"
/* settings_backend_mutex_create() is declared here rather than hand-forward-
 * declared: three copies of that declaration had accumulated across this file
 * and the two firmware main.c files, and a signature drift between them is a
 * link-time-only failure. Implemented in settings_backend_file.c; must run
 * before the scheduler starts (main() is still single-threaded there) so the
 * mutex exists before any task's first settings_lock() call. */
#include "settings_backend.h"
/* tags_port_mutex_create() -- same reasoning, mirrored for the point table's
 * lock. Implemented in tags_port_sim.c; must also run before the scheduler
 * starts so the mutex exists before any task's first tags_lock() call (the
 * decoder tasks that will call tags_publish() run from the very first
 * scheduler tick). */
#include "tags_port.h"

#ifdef PRODUCER_SOCKET
extern void socket_transport_task(void *pv);
#endif

#include <stdio.h>
#include <stdlib.h>

/* Platform seam declared in ui.h (src/ui/), implemented once per platform.
 * The sim has no board to restart -- exit(0) is its stand-in for a reset, and
 * matches Task 16's bench expectation ("the sim exits"). Never returns. */
void platform_request_reset(void)
{
    printf("[PLATFORM] reset requested -- exiting (sim stand-in for a reboot)\n");
    fflush(stdout);
    exit(0);
}

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
    settings_init();

    /* Seed the registry from the persisted selection, mirroring
     * firmware/h757-hmi/CM7/Core/Src/main.c. Without this the config screen's
     * INTERFACE row read transport_active_name() -- which stays pinned at
     * TRANSPORT_DEFAULT forever on this platform -- regardless of what an
     * operator actually saved, since nothing here ever called
     * transport_set_active(). The sim has no real transport hardware to start
     * (dashboard_producer_task/socket_transport_task are the only data
     * sources, selected at build time, not by this registry), so this only
     * makes the displayed selection truthful -- it does not activate anything.
     * A settings blob rejected by settings_validate_interface() leaves the
     * defaults in place, so `active` is always a valid transport_id_t here. */
    {
        settings_interface_t iface;
        settings_get_interface(&iface);
        transport_set_active((transport_id_t)iface.active);
    }

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

/* Commits dirty settings to storage off the UI task. Flash erases can take
 * hundreds of milliseconds on real hardware; doing that on the UI task would
 * freeze LVGL exactly as a modal closes. Polling is fine — settings change at
 * human speed. */
static void settings_writer_task(void *pvParameters)
{
    (void)pvParameters;
    for(;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        settings_commit_if_dirty();
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

    /* Mutexes FIRST. ui_runtime_init() resets the point table, which takes
     * tags_lock() -- and tags_port_sim.c deliberately asserts on a missing
     * mutex rather than degrading to no locking, so the old order (runtime
     * before create) trapped at startup the moment the runtime started
     * touching the table. Still single-threaded here, so creating both up
     * front is free. */
    settings_backend_mutex_create();
    tags_port_mutex_create();
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

    if(xTaskCreate(settings_writer_task, "Set", 2048,
                   NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        return 1;
    }

    vTaskStartScheduler();

    /* Should never reach here. */
    printf("Scheduler returned\n");
    return 0;
}
