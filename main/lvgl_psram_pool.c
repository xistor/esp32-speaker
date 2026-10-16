/**
 * @file lvgl_psram_pool.c
 * @brief Backs LVGL's memory pool with external PSRAM instead of internal DRAM.
 */

#include "lvgl_psram_pool.h"

#include <stdlib.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

static const char *TAG = "lvgl_pool";

/* LVGL's tlsf requires the pool to be aligned to its ALIGN_SIZE: 4 bytes on
 * 32-bit targets, 8 on 64-bit. Request 8 so this stays correct either way. */
#define POOL_ALIGN (8)

void *lvgl_psram_pool_alloc(size_t size)
{
    if (size == 0) {
        ESP_LOGE(TAG, "called with size 0");
        return NULL;
    }

    void *pool = heap_caps_aligned_alloc(POOL_ALIGN, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pool != NULL) {
        ESP_LOGI(TAG, "pool of %u bytes in PSRAM", (unsigned)size);
        return pool;
    }

    /* PSRAM may be absent, not yet brought up, or too fragmented at this point.
     * Falling back to the default-capability heap keeps the UI alive, at the cost
     * of putting the pool back into internal DRAM. */
    ESP_LOGW(TAG, "PSRAM alloc of %u bytes failed, falling back to internal RAM",
             (unsigned)size);

    pool = heap_caps_aligned_alloc(POOL_ALIGN, size, MALLOC_CAP_8BIT);
    if (pool != NULL) {
        ESP_LOGW(TAG, "pool of %u bytes in internal RAM", (unsigned)size);
        return pool;
    }

    /* lv_tlsf_create_with_pool() would write through this pointer, so failing
     * quietly turns an out-of-memory condition into a null dereference. Abort
     * here instead, where the message names the real cause. */
    ESP_LOGE(TAG, "out of memory: no pool of %u bytes", (unsigned)size);
    abort();
}