/**
 * @file lvgl_psram_pool.h
 * @brief Allocates LVGL's tlsf memory pool from external PSRAM.
 *
 * LVGL's builtin stdlib keeps its pool in a static .bss array (work_mem_int in
 * lv_mem_core_builtin.c), which on ESP32 lands in internal DRAM -- the scarcest
 * memory on the part, and the single largest consumer of it in this project.
 *
 * LVGL exposes LV_MEM_POOL_ALLOC / LV_MEM_POOL_INCLUDE for exactly this case
 * ("Can be in external SRAM too"). This pair implements that hook, so the UI
 * heap costs zero DRAM while keeping LVGL's fast builtin tlsf sub-allocator.
 *
 * Wired up in the top-level CMakeLists.txt:
 *   -DLV_MEM_POOL_INCLUDE="lvgl_psram_pool.h"
 *   -DLV_MEM_POOL_ALLOC=lvgl_psram_pool_alloc
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate the LVGL tlsf pool.
 *
 * Called once, from lv_mem_init(), with LV_MEM_SIZE. Must return a pointer to
 * at least `size` usable bytes aligned to at least 8 (LVGL's tlsf ALIGN_SIZE is
 * 4 on 32-bit targets and 8 on 64-bit), or NULL if the allocation failed.
 *
 * The returned block is never freed by LVGL -- lv_mem_deinit() tears down the
 * tlsf control structure but not the pool itself.
 */
void *lvgl_psram_pool_alloc(size_t size);

#ifdef __cplusplus
}
#endif