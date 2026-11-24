# Getting started

You need ESP-IDF 6.0 or newer. Tested on ESP32-S3 and ESP32-S31 (esp-emu), builds for ESP32-P4.

## Add it

```sh
git clone --recursive https://github.com/huming2207/esp-quickjs-ng components/esp-quickjs-ng
```

`--recursive` matters: `quickjs/` is a submodule. Forget it and the build can't find `quickjs.c`.

## Run a script

Give JS its own task with a decent stack. Recursion eats C stack, and the JS recursion limit is
worked out from the stack of the task that creates the runtime.

```c
#include "quickjs-libc.h"
#include "qjs_esp.h"

static void js_task(void *arg)
{
    JSRuntime *rt = qjs_esp_new_runtime();
    js_std_init_handlers(rt);
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);          // console.log, print
    js_init_module_std(ctx, "qjs:std");
    js_init_module_os(ctx, "qjs:os");          // os.setTimeout and friends
    qjs_esp_init_module_ws(ctx, "esp:ws");     // whichever modules you want
    qjs_esp_loop_t *loop = qjs_esp_loop_new(ctx);

    JSValue ret = JS_Eval(ctx, src, len, "main.js", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(ret)) {
        js_std_dump_error(ctx);
    } else {
        JS_FreeValue(ctx, ret);
        qjs_esp_loop_run(loop);                // until nothing's left to do
    }

    qjs_esp_loop_free(loop);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    vTaskDelete(NULL);
}

xTaskCreate(js_task, "js", 16 * 1024, NULL, 5, NULL);
```

Order matters: `js_std_init_handlers()` before `qjs_esp_loop_new()`, and `qjs_esp_loop_free()`
before `JS_FreeContext()`.

Module names are whatever you register them as. These docs and the examples use `esp:udp`,
`esp:ws` and `esp:posix-socket`.

`JS_Eval()` wants the source NUL-terminated. If you embed `main.js` with `EMBED_TXTFILES`, the
length is `end - start - 1`.

## Memory

`qjs_esp_new_runtime()` allocates from PSRAM first and falls back to internal RAM
(`CONFIG_QJS_ESP_HEAP_PREFER_PSRAM`). Cap it with `JS_SetMemoryLimit()` if you care. For scripts you
don't trust, use an arena instead: see [Untrusted code](untrusted-code.md).

A bare runtime plus context is about 160 KB.

## Next

- [Event loop](event-loop.md) to hook your own C events into JS.
- [examples/](../examples): `basic`, `network`, `bench`, `guard`, `bytecode`.
