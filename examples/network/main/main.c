/*
 * Bring the network up, then run main.js with the udp / ws / posix-socket
 * modules on the FreeRTOS event loop.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "quickjs-libc.h"
#include "qjs_esp.h"

static const char *TAG = "network";

/*
 * Test helper, not part of the component: CPU time (us) of our tasks and the
 * idle tasks so far. A task that stays blocked doesn't move its counter, which
 * is what lets tickless idle put the chip into light sleep.
 */
static JSValue js_task_times(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    static TaskStatus_t st[32];
    UBaseType_t n = uxTaskGetSystemState(st, 32, NULL);
    JSValue o = JS_NewObject(ctx);
    uint64_t idle = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        const char *name = st[i].pcTaskName;
        if (!strncmp(name, "IDLE", 4)) {
            idle += st[i].ulRunTimeCounter;
        } else if (!strncmp(name, "qjs_", 4) || !strcmp(name, "js")) {
            /* several qjs_ws tasks would collide, the test opens one */
            JS_SetPropertyStr(ctx, o, name, JS_NewFloat64(ctx, st[i].ulRunTimeCounter));
        }
    }
    JS_SetPropertyStr(ctx, o, "idle", JS_NewFloat64(ctx, (double)idle));
    return o;
}

extern const char main_js[] asm("_binary_main_js_start");
extern const char main_js_end[] asm("_binary_main_js_end");

static void js_task(void *arg)
{
    JSRuntime *rt = qjs_esp_new_runtime();
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);

    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);
    js_init_module_std(ctx, "qjs:std");
    js_init_module_os(ctx, "qjs:os");
    qjs_esp_init_module_udp(ctx, "esp:udp");
    qjs_esp_init_module_ws(ctx, "esp:ws");
    qjs_esp_init_module_posix_socket(ctx, "esp:posix-socket");
    qjs_esp_loop_t *loop = qjs_esp_loop_new(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "ECHO_HOST", JS_NewString(ctx, CONFIG_EXAMPLE_ECHO_HOST));
    JS_SetPropertyStr(ctx, global, "taskTimes", JS_NewCFunction(ctx, js_task_times, "taskTimes", 0));
    JS_FreeValue(ctx, global);

    JSValue ret = JS_Eval(ctx, main_js, main_js_end - main_js - 1, "main.js", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(ret)) {
        js_std_dump_error(ctx);
    } else {
        JS_FreeValue(ctx, ret);
        qjs_esp_loop_run(loop);
    }

    qjs_esp_loop_free(loop);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    ESP_LOGI(TAG, "done, free heap: %u internal, %u total",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)esp_get_free_heap_size());
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    xTaskCreate(js_task, "js", 16 * 1024, NULL, 5, NULL);
}
