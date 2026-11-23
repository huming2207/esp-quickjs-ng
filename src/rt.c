/*
 * JSRuntime on heap_caps, with a stack limit that matches the FreeRTOS task.
 *
 * SPDX-License-Identifier: MIT
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "qjs_esp_priv.h"

static const char *TAG = "qjs";

#if CONFIG_QJS_ESP_HEAP_PREFER_PSRAM
#define JS_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define JS_CAPS MALLOC_CAP_DEFAULT
#endif

/* _prefer: fall back to internal RAM instead of failing when PSRAM runs dry */
static void *rt_calloc(void *opaque, size_t n, size_t size)
{
    return heap_caps_calloc_prefer(n, size, 2, JS_CAPS, MALLOC_CAP_DEFAULT);
}

static void *rt_malloc(void *opaque, size_t size)
{
    return heap_caps_malloc_prefer(size, 2, JS_CAPS, MALLOC_CAP_DEFAULT);
}

static void rt_free(void *opaque, void *ptr)
{
    heap_caps_free(ptr);
}

static void *rt_realloc(void *opaque, void *ptr, size_t size)
{
    return heap_caps_realloc_prefer(ptr, size, 2, JS_CAPS, MALLOC_CAP_DEFAULT);
}

/* upstream's js__malloc_usable_size() is 0 here, which breaks the memory limit */
static size_t rt_usable_size(const void *ptr)
{
    return ptr ? heap_caps_get_allocated_size((void *)ptr) : 0;
}

static const JSMallocFunctions rt_mf = {
    .js_calloc = rt_calloc,
    .js_malloc = rt_malloc,
    .js_free = rt_free,
    .js_realloc = rt_realloc,
    .js_malloc_usable_size = rt_usable_size,
};

JSRuntime *qjs_esp_new_runtime(void)
{
    /* stack grows down on both xtensa and riscv */
    size_t avail = (uintptr_t)esp_cpu_get_sp() - (uintptr_t)xTaskGetStackStart(NULL);

    if (avail <= CONFIG_QJS_ESP_STACK_MARGIN + 1024) {
        ESP_LOGE(TAG, "task stack too small: %u bytes left", (unsigned)avail);
        return NULL;
    }

    JSRuntime *rt = JS_NewRuntime2(&rt_mf, NULL);
    if (!rt) {
        return NULL;
    }
    /* upstream default is 1 MB, i.e. no protection at all on a 16 KB task */
    JS_SetMaxStackSize(rt, avail - CONFIG_QJS_ESP_STACK_MARGIN);
    return rt;
}
