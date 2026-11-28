/*
 * ESP-IDF glue for QuickJS-NG: runtime helper, FreeRTOS event loop and the
 * udp / ws / posix-socket native modules.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
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

/*
 * Private heap for one runtime, for running code you don't trust to behave.
 * qjs_esp_new_runtime() shares the system heap and falls back to internal RAM;
 * a runtime in an arena can only ever use the arena, and QuickJS throws "out
 * of memory" a little before it's full.
 *
 * Grab it once at boot (e.g. MALLOC_CAP_SPIRAM): it's never freed. Up to 4.
 * Only the JS task may touch it.
 */
typedef struct qjs_esp_arena qjs_esp_arena_t;

qjs_esp_arena_t *qjs_esp_arena_new(size_t size, uint32_t caps);

/* Like qjs_esp_new_runtime(), but every allocation comes from the arena.
 * One runtime per arena at a time: NULL if the arena isn't empty. */
JSRuntime *qjs_esp_new_runtime_in(qjs_esp_arena_t *a);

/*
 * Drop everything in the arena at once, instead of JS_FreeRuntime(). For
 * after a script was stopped or ran out of memory, when the runtime may not
 * free cleanly (JS_FreeRuntime() asserts on any leaked object).
 * Call qjs_esp_loop_free() first: it stops the tasks that point into the
 * arena. Skip js_std_free_handlers / JS_FreeContext / JS_FreeRuntime. Only
 * arena memory comes back: FILEs opened through qjs:std leak.
 */
void qjs_esp_arena_reset(qjs_esp_arena_t *a);

/* Bytes in use now, and the most ever in use since the last reset. */
size_t qjs_esp_arena_used(const qjs_esp_arena_t *a);
size_t qjs_esp_arena_peak(const qjs_esp_arena_t *a);

typedef struct qjs_esp_loop qjs_esp_loop_t;

/* Why qjs_esp_loop_run() returned. Anything but IDLE is final: free the loop. */
typedef enum {
    QJS_ESP_STOP_IDLE = 0,      /* no timers, jobs or open handles left */
    QJS_ESP_STOP_REQUESTED,     /* qjs_esp_loop_stop() */
    QJS_ESP_STOP_DEADLINE,      /* qjs_esp_loop_set_deadline() passed */
    QJS_ESP_STOP_SLICE,         /* one turn ran longer than slice_ms */
    QJS_ESP_STOP_ERROR,         /* an uncatchable error the loop didn't cause, e.g. out of
                                 * memory; qjs_esp_loop_error() has its text */
} qjs_esp_stop_t;

typedef struct {
    /* Longest one turn may run: a callback, or a batch of promise jobs plus
     * one timer. Catches while(1), endless job chains and regexp blowups.
     * 0: no limit. */
    uint32_t slice_ms;
    /* After this much JS without blocking, vTaskDelay(1) so lower priority
     * tasks, the idle task and the task WDT get to run. 0: never. */
    uint32_t yield_ms;
} qjs_esp_limits_t;

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

/* Run until there are no timers, jobs or open handles left, or until it's
 * stopped (see qjs_esp_stop_t). Uncaught errors are printed and the loop keeps
 * going, except uncatchable ones: QuickJS makes every InternalError, "out of
 * memory" among them, uncatchable, and an async function that dies of one never
 * settles its promise. The loop stops with QJS_ESP_STOP_ERROR instead of leaving
 * whoever awaits it waiting. */
qjs_esp_stop_t qjs_esp_loop_run(qjs_esp_loop_t *loop);

/* JS task. Why the loop is stopping, or QJS_ESP_STOP_IDLE (0) while it runs.
 * An uncatchable error seen while this is non-zero is the loop's own interrupt. */
qjs_esp_stop_t qjs_esp_loop_stopping(const qjs_esp_loop_t *loop);

/* JS task. For a host that gets an uncatchable error back from its own
 * JS_Call(): unless the loop is already stopping (then the error is its
 * interrupt), stop it with QJS_ESP_STOP_ERROR and keep the error's text.
 * Running JS is interrupted and qjs_esp_loop_run() returns. */
void qjs_esp_loop_fail(qjs_esp_loop_t *loop, JSValueConst err);

/* JS task. The text of the error behind QJS_ESP_STOP_ERROR; "" otherwise. */
const char *qjs_esp_loop_error(const qjs_esp_loop_t *loop);

/* JS task. Also restarts the current turn's clock, so call it right before
 * evaluating the main script. Default: no limits. */
void qjs_esp_loop_set_limits(qjs_esp_loop_t *loop, const qjs_esp_limits_t *lim);

/* JS task. Absolute esp_timer_get_time() value, 0 for none. Past it, running
 * JS is interrupted and qjs_esp_loop_run() returns QJS_ESP_STOP_DEADLINE. */
void qjs_esp_loop_set_deadline(qjs_esp_loop_t *loop, int64_t deadline_us);

/* Any task, not ISRs. Interrupts running JS, wakes the loop if it's idle,
 * and qjs_esp_loop_run() returns QJS_ESP_STOP_REQUESTED. */
void qjs_esp_loop_stop(qjs_esp_loop_t *loop);

/* JS task. While on, the loop doesn't return when it runs out of work but
 * blocks for the next qjs_esp_loop_post(), e.g. for a host that calls into a
 * script now and then. End it with qjs_esp_loop_stop(). */
void qjs_esp_loop_keep_alive(qjs_esp_loop_t *loop, bool on);

/* What reached an error handler: an exception a timer, promise job or module
 * callback threw, or a promise rejection still unhandled at the end of a turn. */
typedef enum {
    QJS_ESP_UNCAUGHT_ERROR = 0,
    QJS_ESP_UNHANDLED_REJECTION,
} qjs_esp_uncaught_t;

typedef void (*qjs_esp_error_cb_t)(JSContext *ctx, qjs_esp_uncaught_t what, JSValueConst err, void *arg);

/* JS task. cb(ctx, what, err, arg) runs in the JS task for every uncaught
 * error before it's printed to stderr as usual; err is only borrowed. Not
 * called for the interrupt that stops the loop, nor for an uncatchable error
 * (that stops it with QJS_ESP_STOP_ERROR). NULL: print only. */
void qjs_esp_loop_set_error_handler(qjs_esp_loop_t *loop, qjs_esp_error_cb_t cb, void *arg);

/* Close every open handle, stop the tasks, free the loop. The modules' JS
 * callbacks (onrecv, onmessage...) don't fire during teardown, but work you
 * queued with qjs_esp_loop_post() still runs, so it can free its arg; the
 * loop is stopping by then (qjs_esp_loop_stopping() isn't IDLE), so such work
 * should free and not call JS, which would be interrupted anyway. A
 * websocket still connecting holds this up to its connect timeout.
 * Call before JS_FreeContext(). NULL is fine. */
void qjs_esp_loop_free(qjs_esp_loop_t *loop);

/* Queue cb(ctx, arg) to run in the JS task. Any task; not ISRs. */
esp_err_t qjs_esp_loop_post(qjs_esp_loop_t *loop, qjs_esp_work_cb_t cb, void *arg, TickType_t wait);

/*
 * Bytecode container, little endian:
 *   "QJBC", u8 format (1), u8 flags (0, reserved), u16 0, u32 len, then len
 *   bytes of QuickJS bytecode (one ES module).
 * tools/qjs_esp_bc.py makes them on a host. The bytecode only loads on the
 * same QuickJS-NG version that wrote it (BC_VERSION).
 */
#define QJS_ESP_BC_HDR_LEN 12

/* Strip flags for qjs_esp_bc_compile(). Debug info is what gives errors
 * their file:line, keep it unless you're short of flash. */
#define QJS_ESP_BC_STRIP_SOURCE 0x01    /* Function.prototype.toString() source */
#define QJS_ESP_BC_STRIP_DEBUG  0x02    /* line numbers too */

#if CONFIG_QJS_ESP_BC_COMPILE
/* Compile src as an ES module into a container, js_malloc'd (free
 * with js_free). Imports aren't resolved here, only when it's evaluated.
 * NULL with an exception pending on a syntax error. */
uint8_t *qjs_esp_bc_compile(JSContext *ctx, const char *src, size_t len, const char *name,
                            int strip, size_t *out_len);
#endif

#if CONFIG_QJS_ESP_BC_LOAD
/* Check the container, then load, link and run the module, like JS_Eval() of
 * its source: returns the module's promise, or JS_EXCEPTION. A malformed
 * container throws before QuickJS sees any of it, but the bytecode itself is
 * trusted: see CONFIG_QJS_ESP_BC_LOAD.
 * mod (may be NULL) gets the module once it's linked, for
 * JS_GetModuleNamespace(); it lives as long as the context. */
JSValue qjs_esp_bc_eval(JSContext *ctx, const uint8_t *blob, size_t len, JSModuleDef **mod);
#endif

/*
 * setTimeout/setInterval/clearTimeout/clearInterval (needs qjs:os registered
 * first), TextEncoder/TextDecoder (UTF-8 only), AbortController/AbortSignal.
 * Skips anything already defined. 0, or -1 with an exception pending.
 */
int qjs_esp_add_web_globals(JSContext *ctx);

#if CONFIG_QJS_ESP_FETCH
/*
 * fetch(), Headers, Response globals (also does qjs_esp_add_web_globals()).
 * Call after qjs_esp_loop_new(). Differences from the browser one:
 *   - body is buffered whole, max CONFIG_QJS_ESP_FETCH_MAX_BODY_KB; no
 *     streams, so Response.body is null
 *   - request body: string, ArrayBuffer or view only; http(s) only;
 *     statusText is always ''
 *   - no CORS/credentials/cache/mode; redirect: 'manual' returns the 3xx
 *   - abort rejects at once but the request still finishes in the background
 *   - extra `timeout` option (ms)
 * 0, or -1 with an exception pending.
 */
int qjs_esp_init_fetch(JSContext *ctx);
#endif

/* Native modules, same calling convention as js_init_module_std(). */
JSModuleDef *qjs_esp_init_module_udp(JSContext *ctx, const char *name);
JSModuleDef *qjs_esp_init_module_posix_socket(JSContext *ctx, const char *name);
JSModuleDef *qjs_esp_init_module_ws(JSContext *ctx, const char *name);

#ifdef __cplusplus
}
#endif
