/*
 * ESP-IDF glue for QuickJS-NG: runtime helper, FreeRTOS event loop and the
 * udp / ws / posix-socket native modules.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * JS_NewRuntime() with a heap_caps allocator (PSRAM first if enabled in
 * Kconfig) that also reports real block sizes, so JS_SetMemoryLimit() works.
 * Call it from the task that will run JS: the JS recursion limit is derived
 * from that task's stack, minus CONFIG_QJS_ESP_STACK_MARGIN for native code.
 */
JSRuntime *qjs_esp_new_runtime(void);

typedef struct qjs_esp_loop qjs_esp_loop_t;

/* Runs in the JS task. */
typedef void (*qjs_esp_work_cb_t)(JSContext *ctx, void *arg);

/*
 * Event loop. The JS task blocks on a FreeRTOS queue; timers and promise jobs
 * come from quickjs-libc (js_std_loop_once), socket readiness from one shared
 * I/O task, websocket traffic from a task per open connection.
 *
 * Call js_std_init_handlers(rt) first. Uses JS_SetRuntimeOpaque() - one loop
 * per runtime, and the opaque slot is ours.
 *
 * os.setReadHandler()/setWriteHandler() are NOT serviced here (that's the
 * poll() loop in js_std_loop()); use the posix-socket module's poll().
 */
qjs_esp_loop_t *qjs_esp_loop_new(JSContext *ctx);

/* Run until there are no timers, jobs or open handles left. Uncaught errors
 * are printed and the loop keeps going. */
void qjs_esp_loop_run(qjs_esp_loop_t *loop);

/* Close every open handle, stop the tasks, free the loop. JS callbacks are
 * not called during teardown. Call before JS_FreeContext(). */
void qjs_esp_loop_free(qjs_esp_loop_t *loop);

/* Queue cb(ctx, arg) to run in the JS task. Any task; not ISRs. */
esp_err_t qjs_esp_loop_post(qjs_esp_loop_t *loop, qjs_esp_work_cb_t cb, void *arg, TickType_t wait);

/* Native modules, same calling convention as js_init_module_std(). */
JSModuleDef *qjs_esp_init_module_udp(JSContext *ctx, const char *name);
JSModuleDef *qjs_esp_init_module_posix_socket(JSContext *ctx, const char *name);
JSModuleDef *qjs_esp_init_module_ws(JSContext *ctx, const char *name);

#ifdef __cplusplus
}
#endif
