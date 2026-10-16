# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp_speaker)

# Package the data/ directory into a SPIFFS image and register it as a flashable
# artifact. idf.py flash will pick it up automatically when FLASH_IN_PROJECT is set.
# The path is relative to the project directory (where this CMakeLists.txt lives).
spiffs_create_partition_image(storage "${CMAKE_SOURCE_DIR}/data" FLASH_IN_PROJECT)

# Source LVGL's memory pool from external PSRAM rather than letting the builtin
# stdlib place it in a static .bss array in internal DRAM. main/ implements the
# allocator and already pulls in the lvgl component.
if(TARGET __idf_lvgl__lvgl)
    target_compile_definitions(__idf_lvgl__lvgl PRIVATE
        -DLV_MEM_POOL_INCLUDE="lvgl_psram_pool.h"
        -DLV_MEM_POOL_ALLOC=lvgl_psram_pool_alloc)
    target_include_directories(__idf_lvgl__lvgl PRIVATE "${CMAKE_SOURCE_DIR}/main")

    # liblvgl__lvgl.a is scanned before libmain.a, and lv_mem_core_builtin.c is
    # only pulled in on a later pass, so nothing would ask libmain.a for the
    # allocator in time. -u makes it undefined from the start so libmain.a is
    # scanned with a pending reference. Same idiom IDF itself uses throughout
    # (see the -u app_main / -u start_app flags on the link line).
    target_link_options(__idf_lvgl__lvgl INTERFACE -u lvgl_psram_pool_alloc)
endif()
