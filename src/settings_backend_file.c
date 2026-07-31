/* Simulator backend for the portable settings store: one binary file next to
 * the executable, modelling erased flash as 0xFF. Loaded whole on first use and
 * written through on every change — the file is 8 KB, so simplicity wins. */
#include "settings_backend.h"
#include <stdio.h>
#include <string.h>

#define STORE_BYTES 8192u
#define STORE_PATH  "settings.bin"

static unsigned char s_img[STORE_BYTES];
static int           s_loaded;

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
