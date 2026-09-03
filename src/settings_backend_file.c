/* Simulator backend for the portable settings store: one binary file next to
 * the executable, modelling erased flash as 0xFF. Loaded whole on first use and
 * written through on every change — the file is 8 KB, so simplicity wins. */
#include "settings_backend.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORE_BYTES 8192u
#define STORE_PATH  "settings.bin"

/* ---- fault injection (hmi-ui#11) ------------------------------------------
 * The host tests reach the retry cap and the commit diagnostics through
 * src/settings/test/fake_backend.c, but they link no LVGL, so two paths stay
 * out of reach there: the literal "(NOT SAVED)" string in
 * network_settings_dialog.cpp, and attach_summary()'s re-arm-while-unsettled
 * branch, which needs a real LVGL timer and a real screen rebuild.
 *
 * The simulator has both, plus a real preemptive FreeRTOS writer task on Win32
 * threads (see the mutex note at the bottom of this file). So it can prove
 * those two paths with no board and no scratch firmware image -- and they are
 * portable src/ui/ code, identical on the H757, which is what makes that a
 * proof rather than an approximation.
 *
 * Environment-driven rather than a compile-time switch, so the SAME binary the
 * gate built is the one under test: a rebuild between "healthy" and "failing"
 * would leave open which build was actually running.
 *
 *   HMI_SIM_FAIL_WRITES=1   program fails, erase succeeds  <- the dangerous
 *                           mode (docs/settings-storage.md 5.1): the store is
 *                           already erased when the write is refused
 *   HMI_SIM_FAIL_ERASES=1   erase fails, so a commit cannot even reach program
 *   HMI_SIM_FAULT_DELAY_MS  per-attempt delay, default 0
 *
 * Names mirror g_fake_fail_writes / g_fake_fail_erases in fake_backend.c on
 * purpose -- same two modes, same meaning, two different harnesses.
 *
 * The delay exists because the sim's writer task polls every 200 ms and the cap
 * is 5 retries, so a failing commit settles in about a second -- far too fast to
 * navigate away and back inside, which is precisely what the re-arm branch needs.
 * It is not a cheat: the writer task's own comment says flash erases take
 * hundreds of milliseconds on real hardware, so a slow backend is the realistic
 * case and an instant one is the artificial one. */
static int s_fault_resolved;
static int s_fail_writes;
static int s_fail_erases;
static int s_fault_delay_ms;

static int env_flag(const char *name)
{
    const char *v = getenv(name);
    return (v != NULL && v[0] != '\0' && v[0] != '0');
}

static void resolve_faults_once(void)
{
    if (s_fault_resolved) return;
    s_fault_resolved = 1;
    s_fail_writes = env_flag("HMI_SIM_FAIL_WRITES");
    s_fail_erases = env_flag("HMI_SIM_FAIL_ERASES");
    {
        const char *d = getenv("HMI_SIM_FAULT_DELAY_MS");
        s_fault_delay_ms = (d != NULL) ? atoi(d) : 0;
        if (s_fault_delay_ms < 0) s_fault_delay_ms = 0;
    }
    /* Said out loud, once. A fault-injected run that is mistaken for a healthy
     * one produces a "settings are broken" bug report against good code. */
    if (s_fail_writes || s_fail_erases || s_fault_delay_ms) {
        printf("[SET] FAULT INJECTION ACTIVE: writes=%d erases=%d delay=%dms "
               "-- commits will fail on purpose (hmi-ui#11)\n",
               s_fail_writes, s_fail_erases, s_fault_delay_ms);
        fflush(stdout);
    }
}

static unsigned char s_img[STORE_BYTES];
static int           s_loaded;

/* Only ever reached from the writer task (settings_backend_write/erase are
 * called by settings_commit_if_dirty), so blocking here is safe and models a
 * slow flash part rather than stalling the UI. */
static void fault_delay(void)
{
    if (s_fault_delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(s_fault_delay_ms));
}

static void load_once(void)
{
    if (s_loaded) return;
    s_loaded = 1;
    memset(s_img, 0xFF, sizeof(s_img));
    FILE *f = fopen(STORE_PATH, "rb");
    if (f) { fread(s_img, 1, sizeof(s_img), f); fclose(f); }
}

static int flush(void)
{
    FILE *f = fopen(STORE_PATH, "wb");
    if (!f) return -1;
    size_t n = fwrite(s_img, 1, sizeof(s_img), f);
    fclose(f);
    return (n == sizeof(s_img)) ? 0 : -1;
}

int settings_backend_read(uint32_t off, void *buf, uint32_t len)
{
    load_once();
    if (!settings_backend_range_ok(off, len, STORE_BYTES)) return -1;
    memcpy(buf, s_img + off, len);
    return 0;
}

int settings_backend_write(uint32_t off, const void *buf, uint32_t len)
{
    load_once();
    if (!settings_backend_range_ok(off, len, STORE_BYTES)) return -1;
    /* Checked AFTER the range check so the injected fault cannot mask a real
     * contract violation, and BEFORE the memcpy so the in-RAM image stays
     * consistent with what a refusing flash part would hold. */
    resolve_faults_once();
    fault_delay();
    if (s_fail_writes) return -1;
    memcpy(s_img + off, buf, len);
    return flush();
}

/* Contract (settings_backend.h): zero-length is a no-op, offset and length must
 * be 4K-aligned. Enforced here even though a file has no subsectors, so all four
 * backends reject the same inputs and the simulator can't accept something the
 * H757's QSPI would refuse. */
int settings_backend_erase(uint32_t off, uint32_t len)
{
    load_once();
    if (len == 0u) return 0;
    if (!settings_backend_range_ok(off, len, STORE_BYTES)) return -1;
    if ((off & 0xFFFu) != 0u || (len & 0xFFFu) != 0u) return -1;
    resolve_faults_once();
    fault_delay();
    if (s_fail_erases) return -1;
    memset(s_img + off, 0xFF, len);
    return flush();
}

/* The sim's UI task (settings_set_network) and writer task
 * (settings_commit_if_dirty) are real preemptive Win32 threads under the
 * kernel's MSVC-MinGW Windows simulator port — not cooperative scheduling.
 * Without a real mutex the writer task can serialize g_network in settings.c
 * while the UI task is mid-assignment through settings_set_network,
 * persisting a torn address. So this is a real FreeRTOS mutex, matching the
 * firmware backends (settings_backend_ram.c, settings_backend_qspi.c).
 *
 * The mutex is created once, from main() before the scheduler starts (see
 * settings_backend_mutex_create() and its call site in freertos_main.c) --
 * single-threaded at that point, so there is no create-vs-first-use race. */

#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t s_mtx;

void settings_backend_mutex_create(void)
{
    s_mtx = xSemaphoreCreateMutex();
}

void settings_lock(void)
{
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
}

void settings_unlock(void)
{
    if (s_mtx) xSemaphoreGive(s_mtx);
}
