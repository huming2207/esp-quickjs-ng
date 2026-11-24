/*
 * JSRuntime on heap_caps or on a private arena, with a stack limit that
 * matches the FreeRTOS task.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "multi_heap.h"
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

static JSRuntime *new_runtime(const JSMallocFunctions *mf, void *opaque)
{
    /* stack grows down on both xtensa and riscv */
    size_t avail = (uintptr_t)esp_cpu_get_sp() - (uintptr_t)xTaskGetStackStart(NULL);

    if (avail <= CONFIG_QJS_ESP_STACK_MARGIN + 1024) {
        ESP_LOGE(TAG, "task stack too small: %u bytes left", (unsigned)avail);
        return NULL;
    }

    JSRuntime *rt = JS_NewRuntime2(mf, opaque);
    if (!rt) {
        return NULL;
    }
    /* upstream default is 1 MB, i.e. no protection at all on a 16 KB task */
    JS_SetMaxStackSize(rt, avail - CONFIG_QJS_ESP_STACK_MARGIN);
    return rt;
}

JSRuntime *qjs_esp_new_runtime(void)
{
    return new_runtime(&rt_mf, NULL);
}

/* ---- arena: a private TLSF heap, so JS can't eat anybody else's RAM ----
 * No lock: only the JS task allocates, same as QuickJS itself. */

struct qjs_esp_arena {
    void *mem;
    size_t size;
    multi_heap_handle_t heap;
    size_t empty_free;  /* free bytes right after (re)registering */
    JSRuntime *rt;      /* the runtime living here; may be stale once freed */
    qjs_esp_loop_t *loop;   /* its loop, for the budget check on allocation */
    uint8_t n_alloc;
};

static qjs_esp_arena_t *arenas[4];

qjs_esp_arena_t *qjs_esp_arena_new(size_t size, uint32_t caps)
{
    int slot = 0;
    while (slot < countof(arenas) && arenas[slot]) {
        slot++;
    }
    if (slot == countof(arenas)) {
        ESP_LOGE(TAG, "too many arenas");
        return NULL;
    }
    qjs_esp_arena_t *a = heap_caps_calloc(1, sizeof(*a), MALLOC_CAP_DEFAULT);
    void *mem = heap_caps_malloc(size, caps);
    if (!a || !mem) {
        ESP_LOGE(TAG, "no room for a %u byte arena", (unsigned)size);
        heap_caps_free(a);
        heap_caps_free(mem);
        return NULL;
    }
    a->mem = mem;
    a->size = size;
    qjs_esp_arena_reset(a);
    arenas[slot] = a;
    return a;
}

void qjs_esp_arena_reset(qjs_esp_arena_t *a)
{
    a->heap = multi_heap_register(a->mem, a->size);
    a->empty_free = multi_heap_free_size(a->heap);
    a->rt = NULL;
    a->loop = NULL;
}

qjs_esp_arena_t *qjs_esp_arena_of(JSRuntime *rt)
{
    for (int i = 0; i < countof(arenas) && arenas[i]; i++) {
        if (arenas[i]->rt == rt) {
            return arenas[i];
        }
    }
    return NULL;
}

void qjs_esp_arena_bind(qjs_esp_arena_t *a, qjs_esp_loop_t *l)
{
    a->loop = l;
}

/*
 * QuickJS only polls for interrupts every 10000 bytecode branches and calls.
 * Builtins (fill, sort, JSON, repeat...) don't count, so a loop doing one
 * heavy builtin per iteration would run for many seconds unchecked. Nearly
 * all of those allocate, so check the budget here too. Stop flag every time,
 * the clock every 16th call. Once stopped, allocations fail.
 */
static inline bool over_budget(qjs_esp_arena_t *a)
{
    return a->loop && qjs_esp_loop_check(a->loop, (++a->n_alloc & 15) == 0);
}

size_t qjs_esp_arena_used(const qjs_esp_arena_t *a)
{
    return a->empty_free - multi_heap_free_size(a->heap);
}

size_t qjs_esp_arena_peak(const qjs_esp_arena_t *a)
{
    return a->empty_free - multi_heap_minimum_free_size(a->heap);
}

static void *ar_malloc(void *opaque, size_t size)
{
    qjs_esp_arena_t *a = opaque;
    return over_budget(a) ? NULL : multi_heap_malloc(a->heap, size);
}

static void *ar_calloc(void *opaque, size_t n, size_t size)
{
    qjs_esp_arena_t *a = opaque;
    if ((size && n > SIZE_MAX / size) || over_budget(a)) {
        return NULL;
    }
    void *p = multi_heap_malloc(a->heap, n * size);
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}

static void ar_free(void *opaque, void *ptr)
{
    multi_heap_free(((qjs_esp_arena_t *)opaque)->heap, ptr);
}

static void *ar_realloc(void *opaque, void *ptr, size_t size)
{
    qjs_esp_arena_t *a = opaque;
    return over_budget(a) ? NULL : multi_heap_realloc(a->heap, ptr, size);
}

/* QuickJS doesn't pass the opaque here, so find the arena by address.
 * Arenas are never freed, the table only grows. */
static size_t ar_usable_size(const void *ptr)
{
    for (int i = 0; ptr && i < countof(arenas) && arenas[i]; i++) {
        const qjs_esp_arena_t *a = arenas[i];
        if ((uintptr_t)ptr - (uintptr_t)a->mem < a->size) {
            return multi_heap_get_allocated_size(a->heap, (void *)ptr);
        }
    }
    return 0;
}

static const JSMallocFunctions ar_mf = {
    .js_calloc = ar_calloc,
    .js_malloc = ar_malloc,
    .js_free = ar_free,
    .js_realloc = ar_realloc,
    .js_malloc_usable_size = ar_usable_size,
};

JSRuntime *qjs_esp_new_runtime_in(qjs_esp_arena_t *a)
{
    /* Leftovers are either a live runtime or a leak. Both are the caller's bug. */
    if (qjs_esp_arena_used(a)) {
        ESP_LOGE(TAG, "arena not empty (%u bytes): free the runtime or reset the arena",
                 (unsigned)qjs_esp_arena_used(a));
        return NULL;
    }
    JSRuntime *rt = new_runtime(&ar_mf, a);
    if (!rt) {
        return NULL;
    }
    /* Stop QuickJS a bit short of the end: it then throws a normal "out of
     * memory" while TLSF still has room for the error object and unwinding,
     * and fragmentation doesn't bite first. */
    size_t headroom = a->empty_free / 16 > 32768 ? a->empty_free / 16 : 32768;
    JS_SetMemoryLimit(rt, a->empty_free > headroom * 2 ? a->empty_free - headroom : a->empty_free / 2);
    /* a stale rt from a freed runtime points into this arena's memory, so it
     * can't match a runtime living anywhere else */
    a->rt = rt;
    a->loop = NULL;
    return rt;
}
