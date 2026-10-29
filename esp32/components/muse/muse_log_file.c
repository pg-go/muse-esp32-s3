/*
 * See muse_log_file.h. One esp_log vprintf hook, chained to the
 * previous one (diagnostic_log / UART). No log line is ever
 * dropped for the file's sake: if the mutex, the ring, or the
 * file is unavailable, the line still goes to the console.
 */

#include "muse_log_file.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOG_PATH "/sdcard/log.txt"
#define RING_SIZE (96 * 1024)
#define LOG_LINE_MAX 1024
#define MAX_FILE_SIZE (512 * 1024)

static vprintf_like_t s_prev;
static SemaphoreHandle_t s_lock;
static uint8_t *s_ring;
static size_t s_ring_head; /* next write position */
static size_t s_ring_len;  /* bytes held, <= RING_SIZE */
static FILE *s_file;
static bool s_file_failed;
static bool s_in_file_write;

static void ring_push(const char *data, size_t len)
{
    if (!s_ring) return;
    for (size_t i = 0; i < len; i++) {
        s_ring[s_ring_head] = (uint8_t)data[i];
        s_ring_head = (s_ring_head + 1) % RING_SIZE;
        if (s_ring_len < RING_SIZE) s_ring_len++;
    }
}

/* fflush alone is NOT enough on FatFs: it only pushes the stdio
 * buffer into the VFS, but the directory entry (file size) is
 * not updated until f_sync/close. A card pulled live therefore
 * shows log.txt as 0 bytes even though sectors were written.
 * fsync() (FatFs f_sync) updates the directory entry, so a
 * live pull actually shows the content. This was the bug that
 * made the first TF-log build report an empty log.txt. */
static bool sync_file(void)
{
    if (!s_file) return false;
    if (fflush(s_file) != 0) return false;
    if (fsync(fileno(s_file)) != 0) return false;
    return true;
}

static void ring_flush_to_file(void)
{
    if (!s_file || !s_ring || s_ring_len == 0) return;
    size_t start = (s_ring_head + RING_SIZE - s_ring_len) % RING_SIZE;
    size_t first = RING_SIZE - start;
    if (first > s_ring_len) first = s_ring_len;
    fwrite(s_ring + start, 1, first, s_file);
    if (first < s_ring_len) {
        fwrite(s_ring, 1, s_ring_len - first, s_file);
    }
    s_ring_len = 0;
    s_ring_head = 0;
}

static int log_file_vprintf(const char *format, va_list args)
{
    /* Copy BEFORE handing args to the previous sink: a vprintf
     * sink may consume its va_list, and copying afterwards is
     * undefined (in practice it produced empty lines). The
     * diagnostic_log sink happens to copy internally, but this
     * hook must not depend on that. */
    va_list copy;
    va_copy(copy, args);
    /* Console / diagnostic ring first, with the original args. */
    int ret = 0;
    if (s_prev) {
        ret = s_prev(format, args);
    } else {
        ret = vprintf(format, args);
    }
    if (s_in_file_write) {
        va_end(copy);
        return ret;
    }
    /* Format once into a static buffer; the mutex makes this safe
     * across tasks, and a 10 ms wait keeps a contended log call
     * from stalling the caller (the line is then console-only). */
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) != pdTRUE) {
        va_end(copy);
        return ret;
    }
    static char line[LOG_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        if (s_file && !s_file_failed) {
            s_in_file_write = true;
            size_t written = fwrite(line, 1, len, s_file);
            if (written != len || !sync_file()) {
                /* Card pulled / write error: stop trying, keep
                 * the console path alive. A reboot restarts it. */
                s_file_failed = true;
                fclose(s_file);
                s_file = NULL;
            }
            s_in_file_write = false;
        } else {
            ring_push(line, len);
        }
    }
    xSemaphoreGive(s_lock);
    return ret;
}

void muse_log_file_install(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;
    s_ring = heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        /* No PSRAM ring: lines still go to the file once mounted,
         * just without the pre-mount history. */
    }
    s_prev = esp_log_set_vprintf(log_file_vprintf);
}

void muse_log_file_start(void)
{
    if (!s_lock || s_file || s_file_failed) return;
    struct stat st;
    const char *mode = "a";
    if (stat(LOG_PATH, &st) == 0 && st.st_size > MAX_FILE_SIZE) {
        mode = "w";
    }
    FILE *f = fopen(LOG_PATH, mode);
    if (!f) {
        s_file_failed = true;
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        fclose(f);
        return;
    }
    s_file = f;
    fprintf(s_file, "\n=== boot log started (TF mounted) ===\n");
    ring_flush_to_file();
    if (!sync_file()) {
        /* Header/ring did not land: mark failed rather than
         * silently accumulating an invisible file. */
        s_file_failed = true;
        fclose(s_file);
        s_file = NULL;
    }
    xSemaphoreGive(s_lock);
}
