/*
 * Tiny JS benchmark: wall time and CPU cycles per workload.
 *
 * SPDX-License-Identifier: MIT
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "quickjs-libc.h"
#include "qjs_esp.h"

static const char *TAG = "bench";

extern const char main_js[] asm("_binary_main_js_start");
extern const char main_js_end[] asm("_binary_main_js_end");

/* cycles() -> CPU cycle counter (32-bit, wraps; keep workloads short) */
static JSValue js_cycles(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return JS_NewUint32(ctx, esp_cpu_get_cycle_count());
}

static void js_task(void *arg)
{
    JSRuntime *rt = qjs_esp_new_runtime();
    js_std_init_handlers(rt);
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);
    js_init_module_os(ctx, "qjs:os");
    qjs_esp_loop_t *loop = qjs_esp_loop_new(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "cycles", JS_NewCFunction(ctx, js_cycles, "cycles", 0));
    JS_FreeValue(ctx, global);

    JSValue ret = JS_Eval(ctx, main_js, main_js_end - main_js - 1, "main.js", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(ret)) {
        js_std_dump_error(ctx);
    } else {
        JS_FreeValue(ctx, ret);
        qjs_esp_loop_run(loop);
    }

    JSMemoryUsage mu;
    JS_ComputeMemoryUsage(rt, &mu);
    ESP_LOGI(TAG, "JS heap: %lld bytes in %lld blocks", (long long)mu.malloc_size, (long long)mu.malloc_count);

    qjs_esp_loop_free(loop);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    ESP_LOGI(TAG, "done");
    vTaskDelete(NULL);
}

void app_main(void)
{
    xTaskCreate(js_task, "js", 16 * 1024, NULL, 5, NULL);
}
