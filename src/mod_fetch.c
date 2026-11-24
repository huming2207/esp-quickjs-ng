/*
 * fetch(), Headers, Response on top of esp_http_client.
 *
 * esp_http_client blocks, so each request gets its own task and stays a loop
 * handle until its result is back on the JS task. It can't be interrupted
 * either: an aborted fetch rejects right away but the request runs to the end
 * and the result gets dropped. Loop teardown waits for those (max: timeout).
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdkconfig.h"
#if CONFIG_QJS_ESP_FETCH

#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "quickjs-libc.h"
#include "qjs_esp.h"
#include "qjs_esp_priv.h"

static const char *TAG = "qjs_fetch";

#define MAX_BODY ((size_t)CONFIG_QJS_ESP_FETCH_MAX_BODY_KB * 1024)
#define MAX_REDIRECTS 20    /* same as browsers */

typedef struct {
    char *p;
    size_t len, cap;
} buf_t;

typedef struct {
    qjs_esp_handle_t h;         /* ref'd until deliver() */
    JSContext *ctx;
    qjs_esp_loop_t *loop;
    JSValue funcs[2];           /* resolve, reject */
    TaskHandle_t task;
    volatile bool detached;     /* loop is going away, deliver() calls nothing */

    /* everything below is off the JS heap, the task reads it */
    char *url;
    esp_http_client_method_t method;
    buf_t req_headers;          /* "name\0value\0"... */
    uint8_t *body;
    size_t body_len;
    int timeout_ms;
    bool follow;

    int status;
    int redirects;
    char *final_url;
    buf_t headers;              /* final response only, not the redirects' */
    buf_t data;
    bool too_big;
    esp_err_t err;
} req_t;

static int inflight;    /* JS task only */

/* PSRAM if there is any, never the JS heap */

static void *mem_realloc(void *p, size_t n)
{
    return heap_caps_realloc_prefer(p, n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

static bool buf_add(buf_t *b, const void *src, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n) {
            cap *= 2;
        }
        char *p = mem_realloc(b->p, cap);
        if (!p) {
            return false;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
    return true;
}

static bool buf_add_str(buf_t *b, const char *s)
{
    return buf_add(b, s, strlen(s) + 1);
}

static void buf_free(buf_t *b)
{
    heap_caps_free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

static void req_free(req_t *r)
{
    heap_caps_free(r->url);
    heap_caps_free(r->body);
    heap_caps_free(r->final_url);
    buf_free(&r->req_headers);
    buf_free(&r->headers);
    buf_free(&r->data);
    heap_caps_free(r);
}


static esp_err_t on_event(esp_http_client_event_t *e)
{
    req_t *r = e->user_data;
    switch (e->event_id) {
    case HTTP_EVENT_ON_STATUS_CODE:
        /* new response: drop whatever the redirect sent */
        r->headers.len = 0;
        r->data.len = 0;
        r->too_big = false;
        break;
    case HTTP_EVENT_ON_HEADER:
        if (!buf_add_str(&r->headers, e->header_key) || !buf_add_str(&r->headers, e->header_value)) {
            r->err = ESP_ERR_NO_MEM;
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (r->too_big || r->err) {
            break;
        }
        if (r->data.len + e->data_len > MAX_BODY) {
            r->too_big = true;  /* can't abort from here, just stop keeping it */
        } else if (!buf_add(&r->data, e->data, e->data_len)) {
            r->err = ESP_ERR_NO_MEM;
        }
        break;
    case HTTP_EVENT_REDIRECT:
        /* auto-redirect is off so we can count them and honour redirect: 'manual'/'error' */
        if (r->follow) {
            r->redirects++;
            esp_http_client_set_redirection(e->client);
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void deliver(JSContext *ctx, void *arg);

static void req_task(void *arg)
{
    req_t *r = arg;
    esp_http_client_config_t cfg = {
        .url = r->url,
        .method = r->method,
        .timeout_ms = r->timeout_ms,
        .disable_auto_redirect = true,
        .max_redirection_count = MAX_REDIRECTS,
        .event_handler = on_event,
        .user_data = r,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        r->err = ESP_ERR_NO_MEM;
    } else {
        for (size_t i = 0; i < r->req_headers.len;) {
            const char *k = r->req_headers.p + i;
            const char *v = k + strlen(k) + 1;
            esp_http_client_set_header(c, k, v);
            i = (v + strlen(v) + 1) - r->req_headers.p;
        }
        if (r->body_len) {
            esp_http_client_set_post_field(c, (const char *)r->body, (int)r->body_len);
        }
        esp_err_t err = esp_http_client_perform(c);
        if (!r->err) {
            r->err = err;
        }
        r->status = esp_http_client_get_status_code(c);
        char url[512];
        if (esp_http_client_get_url(c, url, sizeof(url)) == ESP_OK) {
            r->final_url = mem_realloc(NULL, strlen(url) + 1);
            if (r->final_url) {
                strcpy(r->final_url, url);
            }
        }
        esp_http_client_cleanup(c);
    }
    qjs_esp_loop_post(r->loop, deliver, r, portMAX_DELAY);
    vTaskSuspend(NULL);     /* deliver() deletes us */
}


static JSValue result_obj(JSContext *ctx, req_t *r)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "status", JS_NewInt32(ctx, r->status));
    JS_SetPropertyStr(ctx, o, "url", JS_NewString(ctx, r->final_url ? r->final_url : r->url));
    JS_SetPropertyStr(ctx, o, "redirected", JS_NewBool(ctx, r->redirects > 0));
    JSValue hs = JS_NewArray(ctx);
    uint32_t n = 0;
    for (size_t i = 0; i < r->headers.len;) {
        const char *k = r->headers.p + i;
        const char *v = k + strlen(k) + 1;
        JSValue pair = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, pair, 0, JS_NewString(ctx, k));
        JS_SetPropertyUint32(ctx, pair, 1, JS_NewString(ctx, v));
        JS_SetPropertyUint32(ctx, hs, n++, pair);
        i = (v + strlen(v) + 1) - r->headers.p;
    }
    JS_SetPropertyStr(ctx, o, "headers", hs);
    JS_SetPropertyStr(ctx, o, "body", JS_NewArrayBufferCopy(ctx, (const uint8_t *)r->data.p, r->data.len));
    return o;
}

static void deliver(JSContext *ctx, void *arg)
{
    req_t *r = arg;
    vTaskDeleteWithCaps(r->task);
    r->task = NULL;
    inflight--;

    if (!r->detached) {
        JSValue v;
        int which;
        if (r->err == ESP_OK && !r->too_big) {
            v = result_obj(ctx, r);
            which = 0;
        } else {
            char msg[96];
            if (r->too_big) {
                snprintf(msg, sizeof(msg), "fetch: response body over %u KiB", (unsigned)CONFIG_QJS_ESP_FETCH_MAX_BODY_KB);
            } else {
                snprintf(msg, sizeof(msg), "fetch failed: %s", esp_err_to_name(r->err));
            }
            JS_ThrowTypeError(ctx, "%s", msg);
            v = JS_GetException(ctx);
            which = 1;
        }
        JSValue ret = JS_Call(ctx, r->funcs[which], JS_UNDEFINED, 1, &v);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, r->funcs[0]);
    JS_FreeValue(ctx, r->funcs[1]);
    qjs_esp_handle_unref(ctx, &r->h);
    req_free(r);
}

static void req_detach(qjs_esp_handle_t *h)
{
    req_t *r = container_of(h, req_t, h);
    r->detached = true;
}

/* fetch(method, url, [[name, value]...], body | null, timeoutMs, follow) */

static const struct {
    const char *name;
    esp_http_client_method_t m;
} methods[] = {
    { "GET", HTTP_METHOD_GET }, { "POST", HTTP_METHOD_POST }, { "PUT", HTTP_METHOD_PUT },
    { "PATCH", HTTP_METHOD_PATCH }, { "DELETE", HTTP_METHOD_DELETE }, { "HEAD", HTTP_METHOD_HEAD },
    { "OPTIONS", HTTP_METHOD_OPTIONS },
};

static int copy_str(JSContext *ctx, JSValueConst v, char **out)
{
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, v);
    if (!s) {
        return -1;
    }
    *out = mem_realloc(NULL, len + 1);
    if (*out) {
        memcpy(*out, s, len + 1);
    }
    JS_FreeCString(ctx, s);
    if (!*out) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static int copy_body(JSContext *ctx, JSValueConst v, req_t *r)
{
    if (JS_IsNull(v) || JS_IsUndefined(v)) {
        return 0;
    }
    const uint8_t *src;
    size_t len;
    const char *s = NULL;
    if (JS_IsString(v)) {
        s = JS_ToCStringLen(ctx, &len, v);
        src = (const uint8_t *)s;
    } else {
        size_t off = 0, bpe;
        JSValue ab = JS_IsArrayBuffer(v) ? JS_DupValue(ctx, v) : JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
        if (JS_IsException(ab)) {
            return -1;
        }
        size_t ab_len;
        uint8_t *p = JS_GetArrayBuffer(ctx, &ab_len, ab);
        JS_FreeValue(ctx, ab);
        if (JS_IsArrayBuffer(v)) {
            len = ab_len;
        }
        src = p ? p + off : NULL;
    }
    if (!src && len) {
        return -1;
    }
    if (len) {
        r->body = mem_realloc(NULL, len);
        if (r->body) {
            memcpy(r->body, src, len);
            r->body_len = len;
        }
    }
    if (s) {
        JS_FreeCString(ctx, s);
    }
    if (len && !r->body) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static int copy_headers(JSContext *ctx, JSValueConst arr, req_t *r)
{
    int64_t n = 0;
    if (JS_GetLength(ctx, arr, &n)) {
        return -1;
    }
    for (int64_t i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyInt64(ctx, arr, i);
        JSValue k = JS_GetPropertyUint32(ctx, pair, 0);
        JSValue v = JS_GetPropertyUint32(ctx, pair, 1);
        const char *ks = JS_ToCString(ctx, k);
        const char *vs = ks ? JS_ToCString(ctx, v) : NULL;
        bool ok = ks && vs && buf_add_str(&r->req_headers, ks) && buf_add_str(&r->req_headers, vs);
        JS_FreeCString(ctx, ks);
        JS_FreeCString(ctx, vs);
        JS_FreeValue(ctx, k);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, pair);
        if (!ok) {
            if (!JS_HasException(ctx)) {
                JS_ThrowOutOfMemory(ctx);
            }
            return -1;
        }
    }
    return 0;
}

static JSValue js_fetch_native(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_esp_loop_t *loop = qjs_esp_loop_get(ctx);
    if (!loop) {
        return JS_ThrowInternalError(ctx, "fetch: no event loop (qjs_esp_loop_new)");
    }
    if (inflight >= CONFIG_QJS_ESP_FETCH_MAX_INFLIGHT) {
        return JS_ThrowTypeError(ctx, "fetch: %d requests already running (CONFIG_QJS_ESP_FETCH_MAX_INFLIGHT)",
                                 CONFIG_QJS_ESP_FETCH_MAX_INFLIGHT);
    }

    const char *method = JS_ToCString(ctx, argv[0]);
    if (!method) {
        return JS_EXCEPTION;
    }
    int mi = -1;
    for (size_t i = 0; i < countof(methods); i++) {
        if (!strcmp(method, methods[i].name)) {
            mi = i;
        }
    }
    if (mi < 0) {
        JS_ThrowTypeError(ctx, "fetch: method %s isn't supported", method);
        JS_FreeCString(ctx, method);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, method);

    req_t *r = heap_caps_calloc_prefer(1, sizeof(*r), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!r) {
        return JS_ThrowOutOfMemory(ctx);
    }
    r->method = methods[mi].m;
    int32_t timeout = 0;
    if (copy_str(ctx, argv[1], &r->url) || copy_headers(ctx, argv[2], r) || copy_body(ctx, argv[3], r) ||
        JS_ToInt32(ctx, &timeout, argv[4])) {
        req_free(r);
        return JS_EXCEPTION;
    }
    r->timeout_ms = timeout > 0 ? timeout : CONFIG_QJS_ESP_FETCH_TIMEOUT_MS;
    r->follow = JS_ToBool(ctx, argv[5]);
    r->ctx = ctx;
    r->loop = loop;
    r->funcs[0] = r->funcs[1] = JS_UNDEFINED;

    JSValue promise = JS_NewPromiseCapability(ctx, r->funcs);
    if (JS_IsException(promise)) {
        req_free(r);
        return promise;
    }
    qjs_esp_handle_init(&r->h, req_detach);
    qjs_esp_handle_ref(ctx, &r->h, promise);
    if (xTaskCreateWithCaps(req_task, "qjs_fetch", CONFIG_QJS_ESP_FETCH_TASK_STACK, r,
                            CONFIG_QJS_ESP_FETCH_TASK_PRIO, &r->task, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        JS_FreeValue(ctx, r->funcs[0]);
        JS_FreeValue(ctx, r->funcs[1]);
        qjs_esp_handle_unref(ctx, &r->h);
        req_free(r);
        JS_FreeValue(ctx, promise);
        return JS_ThrowTypeError(ctx, "fetch: no memory for the request task");
    }
    inflight++;
    return promise;
}


static const char fetch_src[] =
    "(native, maxRunning) => {\n"
    "const TOKEN = /^[!#$%&'*+.^_`|~0-9a-z-]+$/;\n"
    "const name = (n) => {\n"
    "    n = String(n).toLowerCase();\n"
    "    if (!TOKEN.test(n)) throw new TypeError(`Invalid header name: '${n}'`);\n"
    "    return n;\n"
    "};\n"
    "const value = (v) => {\n"
    "    v = String(v).replace(/^[\\t\\n\\r ]+|[\\t\\n\\r ]+$/g, '');\n"
    "    if (/[\\0\\r\\n]/.test(v)) throw new TypeError('Invalid header value');\n"
    "    return v;\n"
    "};\n"
    "class Headers {\n"
    "    #m = new Map();\n"
    "    constructor(init) {\n"
    "        if (init == null) return;\n"
    "        if (init instanceof Headers) { for (const [k, v] of init) this.append(k, v); return; }\n"
    "        if (typeof init[Symbol.iterator] === 'function') {\n"
    "            for (const p of init) {\n"
    "                const a = [...p];\n"
    "                if (a.length !== 2) throw new TypeError('Headers: each entry needs a name and a value');\n"
    "                this.append(a[0], a[1]);\n"
    "            }\n"
    "            return;\n"
    "        }\n"
    "        if (typeof init !== 'object') throw new TypeError('Headers: bad init');\n"
    "        for (const k of Object.keys(init)) this.append(k, init[k]);\n"
    "    }\n"
    "    append(n, v) { n = name(n); v = value(v); const o = this.#m.get(n); this.#m.set(n, o === undefined ? v : `${o}, ${v}`); }\n"
    "    set(n, v) { this.#m.set(name(n), value(v)); }\n"
    "    get(n) { const v = this.#m.get(name(n)); return v === undefined ? null : v; }\n"
    "    has(n) { return this.#m.has(name(n)); }\n"
    "    delete(n) { this.#m.delete(name(n)); }\n"
    "    *entries() { yield* [...this.#m].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)); }\n"
    "    *keys() { for (const [k] of this.entries()) yield k; }\n"
    "    *values() { for (const [, v] of this.entries()) yield v; }\n"
    "    forEach(cb, self) { for (const [k, v] of this.entries()) cb.call(self, v, k, this); }\n"
    "    [Symbol.iterator]() { return this.entries(); }\n"
    "    get [Symbol.toStringTag]() { return 'Headers'; }\n"
    "}\n"
    "const bytes = (body) => {\n"
    "    if (body == null) return null;\n"
    "    if (body instanceof ArrayBuffer) return new Uint8Array(body.slice(0));\n"
    "    if (ArrayBuffer.isView(body)) return new Uint8Array(body.buffer.slice(body.byteOffset, body.byteOffset + body.byteLength));\n"
    "    return new TextEncoder().encode(String(body));\n"
    "};\n"
    "const FROM_NATIVE = Symbol('fromNative');\n"
    "class Response {\n"
    "    #body; #used = false; #status; #statusText; #headers; #url = ''; #redirected = false; #type = 'default';\n"
    "    constructor(body = null, init = {}) {\n"
    "        const status = init.status ?? 200;\n"
    "        if (init[FROM_NATIVE] !== true && (!Number.isInteger(status) || status < 200 || status > 599))\n"
    "            throw new RangeError(`Response: status ${status} isn't in 200..599`);\n"
    "        this.#status = status;\n"
    "        this.#statusText = String(init.statusText ?? '');\n"
    "        this.#headers = new Headers(init.headers);\n"
    "        this.#body = bytes(body);\n"
    "        if (typeof body === 'string' && !this.#headers.has('content-type'))\n"
    "            this.#headers.set('content-type', 'text/plain;charset=UTF-8');\n"
    "        if (init[FROM_NATIVE] === true) { this.#url = init.url; this.#redirected = init.redirected; this.#type = init.type ?? 'basic'; }\n"
    "    }\n"
    "    get status() { return this.#status; }\n"
    "    get statusText() { return this.#statusText; }\n"
    "    get ok() { return this.#status >= 200 && this.#status < 300; }\n"
    "    get headers() { return this.#headers; }\n"
    "    get url() { return this.#url; }\n"
    "    get redirected() { return this.#redirected; }\n"
    "    get type() { return this.#type; }\n"
    "    get body() { return null; }   /* no ReadableStream in QuickJS */\n"
    "    get bodyUsed() { return this.#used; }\n"
    "    #take() {\n"
    "        if (this.#used) throw new TypeError('Body has already been consumed');\n"
    "        this.#used = true;\n"
    "        return this.#body ?? new Uint8Array(0);\n"
    "    }\n"
    "    async arrayBuffer() { const b = this.#take(); return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength); }\n"
    "    async bytes() { return new Uint8Array(await this.arrayBuffer()); }\n"
    "    async text() { return new TextDecoder().decode(this.#take()); }\n"
    "    async json() { return JSON.parse(await this.text()); }\n"
    "    clone() {\n"
    "        if (this.#used) throw new TypeError('Response.clone: body already used');\n"
    "        return new Response(this.#body, { [FROM_NATIVE]: true, status: this.#status, statusText: this.#statusText,\n"
    "            headers: this.#headers, url: this.#url, redirected: this.#redirected, type: this.#type });\n"
    "    }\n"
    "    static json(data, init = {}) {\n"
    "        const r = new Response(JSON.stringify(data), init);\n"
    "        r.headers.set('content-type', 'application/json');\n"
    "        return r;\n"
    "    }\n"
    "    static error() { return new Response(null, { [FROM_NATIVE]: true, status: 0, url: '', redirected: false, type: 'error' }); }\n"
    "    get [Symbol.toStringTag]() { return 'Response'; }\n"
    "}\n"
    "const REDIRECTS = [301, 302, 303, 307, 308];\n"
    /* over maxRunning: queue instead of failing. Aborting a queued one just drops it */
    "let running = 0;\n"
    "const waiting = [];\n"
    "const slot = (signal) => {\n"
    "    if (running < maxRunning) { running++; return Promise.resolve(); }\n"
    "    return new Promise((resolve, reject) => {\n"
    "        const w = { resolve, reject };\n"
    "        waiting.push(w);\n"
    "        signal?.addEventListener('abort', () => {\n"
    "            const i = waiting.indexOf(w);\n"
    "            if (i >= 0) { waiting.splice(i, 1); reject(signal.reason); }\n"
    "        }, { once: true });\n"
    "    });\n"
    "};\n"
    "const release = () => { const w = waiting.shift(); if (w) w.resolve(); else running--; };\n"
    "async function fetch(input, init = {}) {\n"
    "    const url = String(input !== null && typeof input === 'object' && 'url' in input ? input.url : input);\n"
    "    if (!/^https?:\\/\\//i.test(url)) throw new TypeError(`fetch: only http: and https: URLs, not '${url}'`);\n"
    "    const method = String(init.method ?? 'GET').toUpperCase();\n"
    "    const headers = new Headers(init.headers);\n"
    "    let body = init.body ?? null;\n"
    "    if (body !== null && (method === 'GET' || method === 'HEAD')) throw new TypeError(`fetch: a ${method} request can't have a body`);\n"
    "    if (body !== null && !(body instanceof ArrayBuffer) && !ArrayBuffer.isView(body)) {\n"
    "        body = String(body);\n"
    "        if (!headers.has('content-type')) headers.set('content-type', 'text/plain;charset=UTF-8');\n"
    "    }\n"
    "    const redirect = init.redirect ?? 'follow';\n"
    "    if (!['follow', 'error', 'manual'].includes(redirect)) throw new TypeError(`fetch: bad redirect mode '${redirect}'`);\n"
    "    const signal = init.signal ?? null;\n"
    "    signal?.throwIfAborted();\n"
    "    const timeout = Number(init.timeout ?? 0) | 0;   /* non-standard, ms, 0 = Kconfig default */\n"
    "    const pending = slot(signal).then(() => {\n"
    "        let p;\n"
    "        try { p = native(method, url, [...headers], body, timeout, redirect === 'follow'); }\n"
    "        catch (e) { release(); throw e; }\n"
    "        p.then(release, release);   /* aborted ones hold their slot until they really finish */\n"
    "        return p;\n"
    "    });\n"
    "    const r = !signal ? await pending : await new Promise((resolve, reject) => {\n"
    "        const onAbort = () => reject(signal.reason);\n"
    "        signal.addEventListener('abort', onAbort, { once: true });\n"
    "        pending.then((v) => { signal.removeEventListener('abort', onAbort); resolve(v); },\n"
    "                     (e) => { signal.removeEventListener('abort', onAbort); reject(e); });\n"
    "    });\n"
    "    if (redirect === 'error' && REDIRECTS.includes(r.status)) throw new TypeError(`fetch: redirected (${r.status}) with redirect: 'error'`);\n"
    "    return new Response(method === 'HEAD' ? null : r.body, { [FROM_NATIVE]: true, status: r.status, headers: r.headers,\n"
    "        url: r.url, redirected: r.redirected });\n"
    "}\n"
    "for (const [k, v] of Object.entries({ fetch, Headers, Response }))\n"
    "    if (globalThis[k] === undefined) Object.defineProperty(globalThis, k, { value: v, writable: true, configurable: true });\n"
    "}";

int qjs_esp_init_fetch(JSContext *ctx)
{
    if (!qjs_esp_loop_get(ctx)) {
        JS_ThrowInternalError(ctx, "qjs_esp_init_fetch: create the loop first");
        return -1;
    }
    if (qjs_esp_add_web_globals(ctx)) {     /* bodies need TextEncoder/Decoder */
        return -1;
    }
    JSValue factory = JS_Eval(ctx, fetch_src, sizeof(fetch_src) - 1, "<fetch>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(factory)) {
        return -1;
    }
    JSValue args[2] = { JS_NewCFunction(ctx, js_fetch_native, "fetch", 6), JS_NewInt32(ctx, CONFIG_QJS_ESP_FETCH_MAX_INFLIGHT) };
    JSValue v = JS_Call(ctx, factory, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, factory);
    if (JS_IsException(v)) {
        return -1;
    }
    JS_FreeValue(ctx, v);
    ESP_LOGD(TAG, "fetch ready");
    return 0;
}

#endif
