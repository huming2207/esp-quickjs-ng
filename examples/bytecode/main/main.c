/*
 * Bytecode containers: one compiled on the host at build time (64-bit host,
 * 32-bit chip), one compiled here, and a few broken ones that must be
 * refused. Ends with "BC PASS" or "BC FAIL".
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "quickjs-libc.h"
#include "qjs_esp.h"

extern const uint8_t host_qbc[] asm("_binary_hello_qbc_start");
extern const uint8_t host_qbc_end[] asm("_binary_hello_qbc_end");
extern const char hello_js[] asm("_binary_hello_js_start");
extern const char hello_js_end[] asm("_binary_hello_js_end");

static int fails;

static void check(bool ok, const char *what)
{
    printf("BC %-40s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

/* Fresh runtime; run the container; return globalThis.result, or the error */
static void run(qjs_esp_arena_t *ar, const uint8_t *blob, size_t len, char *out, size_t out_len)
{
    JSRuntime *rt = qjs_esp_new_runtime_in(ar);
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);
    js_init_module_os(ctx, "qjs:os");
    qjs_esp_loop_t *loop = qjs_esp_loop_new(ctx);

    JSValue ret = qjs_esp_bc_eval(ctx, blob, len);
    JSValue r;
    if (JS_IsException(ret)) {
        r = JS_GetException(ctx);
    } else {
        JS_FreeValue(ctx, ret);
        qjs_esp_loop_run(loop);
        JSValue g = JS_GetGlobalObject(ctx);
        r = JS_GetPropertyStr(ctx, g, "result");
        JS_FreeValue(ctx, g);
    }
    const char *s = JS_ToCString(ctx, r);
    snprintf(out, out_len, "%s", s ? s : "?");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, r);

    qjs_esp_loop_free(loop);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static uint8_t *device_compile(qjs_esp_arena_t *ar, size_t *len)
{
    JSRuntime *rt = qjs_esp_new_runtime_in(ar);
    JSContext *ctx = JS_NewContext(rt);
    js_init_module_os(ctx, "qjs:os");   /* compiling loads imports */
    uint8_t *c = qjs_esp_bc_compile(ctx, hello_js, hello_js_end - hello_js - 1, "hello.js", 0, len);
    uint8_t *copy = NULL;
    if (c) {
        copy = heap_caps_malloc(*len, MALLOC_CAP_SPIRAM);
        memcpy(copy, c, *len);
        js_free(ctx, c);
    } else {
        js_std_dump_error(ctx);
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return copy;
}

static void js_task(void *arg)
{
    qjs_esp_arena_t *ar = qjs_esp_arena_new(2 * 1024 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t host_len = host_qbc_end - host_qbc;
    char res[160], ref[160];

    run(ar, host_qbc, host_len, ref, sizeof(ref));
    printf("BC host bytecode says: %s\n", ref);
    check(strstr(ref, "esp-idf fib(15)=610") != NULL, "host-compiled bytecode runs");
    check(strstr(ref, "hello.js:9") != NULL, "line numbers survive");

    size_t dev_len = 0;
    uint8_t *dev = device_compile(ar, &dev_len);
    check(dev != NULL, "compiles on the device");
    if (dev) {
        run(ar, dev, dev_len, res, sizeof(res));
        check(!strcmp(res, ref), "device-compiled bytecode runs the same");
        printf("BC host %u bytes, device %u bytes, %s\n", (unsigned)host_len, (unsigned)dev_len,
               dev_len == host_len && !memcmp(dev, host_qbc, host_len) ? "identical" : "different");
    }

    /* broken containers: refused before (or by) QuickJS's own checks */
    uint8_t *bad = heap_caps_malloc(host_len, MALLOC_CAP_SPIRAM);
    memcpy(bad, host_qbc, host_len);
    bad[0] = 'X';
    run(ar, bad, host_len, res, sizeof(res));
    check(strstr(res, "not a bytecode container") != NULL, "wrong magic refused");

    memcpy(bad, host_qbc, host_len);
    run(ar, bad, host_len - 1, res, sizeof(res));
    check(strstr(res, "bad length") != NULL, "truncated container refused");

    memcpy(bad, host_qbc, host_len);
    bad[QJS_ESP_BC_HDR_LEN] ^= 0x40;   /* BC_VERSION */
    run(ar, bad, host_len, res, sizeof(res));
    printf("BC   other version -> %s\n", res);
    check(strstr(res, "version") != NULL, "other bytecode version refused");

    check(qjs_esp_arena_used(ar) == 0, "arena empty afterwards");
    printf(fails ? "BC FAIL (%d)\n" : "BC PASS\n", fails);
    vTaskDelete(NULL);
}

void app_main(void)
{
    xTaskCreate(js_task, "js", 16 * 1024, NULL, 5, NULL);
}
