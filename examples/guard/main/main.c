/*
 * Hostile scripts against the arena and the loop limits.
 *
 * Each script gets a fresh runtime in one 2 MB arena. After each one:
 *   - it stopped the way we expect (idle, slice, deadline, host stop),
 *   - the arena is empty again (clean free, or reset after a stop),
 *   - the system heap is exactly where it was,
 *   - a low priority task on the same core kept running (yield works).
 * Ends with "GUARD PASS" or "GUARD FAIL".
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "quickjs-libc.h"
#include "qjs_esp.h"

#define ARENA_SIZE (2 * 1024 * 1024)
#define DEADLINE_US 3000000
#define CORE (portNUM_PROCESSORS - 1)

enum { NONE, STOP_AT_300MS };

static const struct {
    const char *name;
    const char *src;
    qjs_esp_stop_t expect;
    int extra;
    uint32_t min_beats;     /* heartbeats the low prio task must get in */
    bool any_stop;          /* only the heap and arena checks count */
    uint32_t max_ms;        /* 0: don't care */
} tests[] = {
    { .name = "normal", .src = "console.log('hello')", .expect = QJS_ESP_STOP_IDLE },
    { .name = "spin", .src = "while (true) {}", .expect = QJS_ESP_STOP_SLICE, .min_beats = 1 },
    { .name = "job chain", .src = "function f() { Promise.resolve().then(f) } f()", .expect = QJS_ESP_STOP_SLICE, .min_beats = 1 },
    { .name = "timer storm",
      .src = "import * as os from 'qjs:os'; os.setInterval(() => { for (let i = 0; i < 2000; i++); }, 0)",
      .expect = QJS_ESP_STOP_DEADLINE, .min_beats = 50 },
    /* "out of memory" is uncatchable: the module's promise is rejected with it and the loop stops */
    { .name = "array bomb", .src = "const a = []; for (;;) a.push(new Array(10000).fill(1))", .expect = QJS_ESP_STOP_ERROR },
    { .name = "string bomb", .src = "let s = 'x'; for (;;) s += s", .expect = QJS_ESP_STOP_ERROR },
    /* near the limit even the catch path runs out, so OOM tends to escape */
    { .name = "oom hoarder", .src = "const k = []; for (;;) { try { k.push(new Uint8Array(65536)) } catch (e) {} }",
      .any_stop = true, .max_ms = 1000 },
    /* native builtins that never poll for interrupts, one per loop iteration */
    { .name = "native: alloc+fill", .src = "for (;;) new Array(100000).fill(0)",
      .expect = QJS_ESP_STOP_SLICE, .max_ms = 1000 },
    /* builtins that neither allocate nor poll: only the interrupt interval catches these.
     * One fill(200k) is ~40 ms on the S31 and ~94 ms on the S3 in esp-emu, slower
     * on silicon; at the default interval (100) a loop of them overruns by ~50
     * calls. The bound only catches a regression to upstream's 10000 (minutes). */
    { .name = "native: fill 2k", .src = "const a = new Array(2000).fill(1); for (;;) a.fill(2)",
      .any_stop = true, .max_ms = 1000 },
    { .name = "native: fill 200k", .src = "const a = new Array(200000).fill(1); for (;;) a.fill(2)",
      .any_stop = true, .max_ms = 10000 },
    { .name = "native: indexOf 500k", .src = "const s = 'x'.repeat(500000); for (;;) s.indexOf('y')",
      .any_stop = true, .max_ms = 10000 },
    { .name = "native: sort", .src = "const a = Array.from({ length: 30000 }, (_, i) => -i); for (;;) a.slice().sort()",
      .any_stop = true, .max_ms = 1000 },
    { .name = "recursion", .src = "function f(n) { return f(n + 1) + 1 } f(0)", .expect = QJS_ESP_STOP_IDLE },
    { .name = "json nest", .src = "JSON.parse('['.repeat(200000))", .expect = QJS_ESP_STOP_IDLE },
    { .name = "redos", .src = "/^(a+)+$/.test('a'.repeat(40) + 'b')", .expect = QJS_ESP_STOP_SLICE },
    { .name = "idle past deadline", .src = "import * as os from 'qjs:os'; os.setTimeout(() => {}, 60000)",
      .expect = QJS_ESP_STOP_DEADLINE },
    { .name = "stopped by host", .src = "import * as os from 'qjs:os'; os.setTimeout(() => {}, 60000)",
      .expect = QJS_ESP_STOP_REQUESTED, .extra = STOP_AT_300MS },
    { .name = "stopped by host 2", .src = "import * as os from 'qjs:os'; os.setTimeout(() => {}, 60000)",
      .expect = QJS_ESP_STOP_REQUESTED, .extra = STOP_AT_300MS },
    { .name = "normal again", .src = "console.log('still alive')", .expect = QJS_ESP_STOP_IDLE },
};

static const char *stop_name[] = { "idle", "requested", "deadline", "slice", "error" };

static volatile uint32_t beats;

static void heartbeat(void *arg)
{
    for (;;) {
        beats++;
        vTaskDelay(1);
    }
}

static void stop_cb(void *arg)
{
    qjs_esp_loop_stop(arg);
}

static qjs_esp_stop_t run_one(qjs_esp_arena_t *ar, const char *src, int extra, size_t *peak)
{
    JSRuntime *rt = qjs_esp_new_runtime_in(ar);
    if (!rt) {
        printf("no runtime\n");
        abort();
    }
    js_std_init_handlers(rt);
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);
    js_init_module_os(ctx, "qjs:os");
    qjs_esp_loop_t *loop = qjs_esp_loop_new(ctx);

    esp_timer_handle_t t = NULL;
    if (extra == STOP_AT_300MS) {
        esp_timer_create_args_t a = { .callback = stop_cb, .arg = loop, .name = "stop" };
        ESP_ERROR_CHECK(esp_timer_create(&a, &t));
        ESP_ERROR_CHECK(esp_timer_start_once(t, 300000));
    }

    qjs_esp_limits_t lim = { .slice_ms = 200, .yield_ms = 20 };
    qjs_esp_loop_set_limits(loop, &lim);
    qjs_esp_loop_set_deadline(loop, esp_timer_get_time() + DEADLINE_US);

    JSValue ret = JS_Eval(ctx, src, strlen(src), "hostile.js", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(ret)) {
        js_std_dump_error(ctx);
    } else {
        JS_FreeValue(ctx, ret);
    }
    qjs_esp_stop_t why = qjs_esp_loop_run(loop);

    if (t) {
        esp_timer_stop(t);      /* already fired: fine */
        esp_timer_delete(t);
    }
    qjs_esp_loop_free(loop);
    *peak = qjs_esp_arena_peak(ar);
    if (why == QJS_ESP_STOP_IDLE) {
        js_std_free_handlers(rt);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);     /* asserts if anything leaked */
    } else {
        qjs_esp_arena_reset(ar);
    }
    vTaskDelay(pdMS_TO_TICKS(20));  /* the I/O task deletes itself; idle frees its stack */
    return why;
}

static void js_task(void *arg)
{
    qjs_esp_arena_t *ar = qjs_esp_arena_new(ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ar) {
        printf("GUARD FAIL: no arena\n");
        vTaskDelete(NULL);
    }

    /* first use of stderr, the eventfd VFS, esp_timer etc. allocates for good */
    size_t peak;
    run_one(ar, "throw new Error('warm-up')", NONE, &peak);
    run_one(ar, "import * as os from 'qjs:os'; os.setTimeout(() => {}, 60000)", STOP_AT_300MS, &peak);

    size_t int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ext0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int fails = 0;

    for (int i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        uint32_t b0 = beats;
        int64_t t0 = esp_timer_get_time();
        qjs_esp_stop_t why = run_one(ar, tests[i].src, tests[i].extra, &peak);
        int64_t ms = (esp_timer_get_time() - t0) / 1000;
        uint32_t b = beats - b0;
        size_t used = qjs_esp_arena_used(ar);
        long dint = (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - (long)int0;
        long dext = (long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - (long)ext0;

        bool ok = (tests[i].any_stop || why == tests[i].expect) && used == 0 && dint == 0 && dext == 0 &&
                  b >= tests[i].min_beats && (!tests[i].max_ms || ms <= tests[i].max_ms);
        fails += !ok;
        printf("GUARD %-19s %-9s %5lld ms  arena peak %4u KB  beats %4u  heap %+ld/%+ld  %s\n",
               tests[i].name, stop_name[why], ms, (unsigned)(peak / 1024), (unsigned)b, dint, dext,
               ok ? "ok" : "FAIL");
    }
    printf(fails ? "GUARD FAIL (%d)\n" : "GUARD PASS\n", fails);
    vTaskDelete(NULL);
}

void app_main(void)
{
    /* same core, lower priority: only runs when JS blocks or yields */
    xTaskCreatePinnedToCore(heartbeat, "beat", 2048, NULL, 1, NULL, CORE);
    xTaskCreatePinnedToCore(js_task, "js", 16 * 1024, NULL, 5, NULL, CORE);
}
