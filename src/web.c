/*
 * The web globals scripts usually assume exist: timers, TextEncoder/Decoder
 * (UTF-8 only), AbortController/AbortSignal. Just enough for fetch() and
 * normal app code, not a browser.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "quickjs-libc.h"
#include "qjs_esp.h"
#include "qjs_esp_priv.h"

/* qjs:os only exports them, nothing puts them on globalThis */
static const char timers_src[] =
    "import * as os from 'qjs:os';\n"
    "for (const k of ['setTimeout', 'clearTimeout', 'setInterval', 'clearInterval'])\n"
    "    if (globalThis[k] === undefined) globalThis[k] = os[k];\n";

static const char classes_src[] =
    "(utf8Encode, utf8Decode) => {\n"
    "const def = (name, v) => {\n"
    "    if (globalThis[name] === undefined)\n"
    "        Object.defineProperty(globalThis, name, { value: v, writable: true, configurable: true });\n"
    "};\n"
    "const UTF8 = ['utf-8', 'utf8', 'unicode-1-1-utf-8'];\n"
    "class TextEncoder {\n"
    "    get encoding() { return 'utf-8'; }\n"
    "    encode(s = '') { return utf8Encode(String(s)); }\n"
    "}\n"
    "class TextDecoder {\n"
    "    #fatal; #ignoreBOM;\n"
    "    constructor(label = 'utf-8', opts = {}) {\n"
    "        if (!UTF8.includes(String(label).trim().toLowerCase()))\n"
    "            throw new RangeError(`TextDecoder: only utf-8 is supported, not '${label}'`);\n"
    "        this.#fatal = !!opts.fatal;\n"
    "        this.#ignoreBOM = !!opts.ignoreBOM;\n"
    "    }\n"
    "    get encoding() { return 'utf-8'; }\n"
    "    get fatal() { return this.#fatal; }\n"
    "    get ignoreBOM() { return this.#ignoreBOM; }\n"
    "    decode(buf) {\n"
    "        if (buf === undefined) return '';\n"
    "        const u8 = buf instanceof ArrayBuffer ? new Uint8Array(buf)\n"
    "            : ArrayBuffer.isView(buf) ? new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength) : null;\n"
    "        if (!u8) throw new TypeError('TextDecoder.decode: needs an ArrayBuffer or a view');\n"
    "        const bom = !this.#ignoreBOM && u8.length >= 3 && u8[0] === 0xef && u8[1] === 0xbb && u8[2] === 0xbf;\n"
    "        return utf8Decode(bom ? u8.subarray(3) : u8, this.#fatal);\n"
    "    }\n"
    "}\n"
    "const abortError = (name, message) => Object.assign(new Error(message), { name });\n"
    "let abortVia;\n"
    "class AbortSignal {\n"
    "    #aborted = false; #reason; #listeners = []; #timeoutMs = 0; #timer = null;\n"
    "    onabort = null;\n"
    "    get aborted() { return this.#aborted; }\n"
    "    get reason() { return this.#reason; }\n"
    "    throwIfAborted() { if (this.#aborted) throw this.#reason; }\n"
    /* only arm the timer while someone listens, or an unused timeout() keeps the loop up */
    "    addEventListener(type, fn, opts) {\n"
    "        if (type !== 'abort' || typeof fn !== 'function' || this.#listeners.some((l) => l.fn === fn)) return;\n"
    "        this.#listeners.push({ fn, once: !!(opts && opts.once) });\n"
    "        if (this.#timeoutMs && !this.#timer && !this.#aborted)\n"
    "            this.#timer = setTimeout(() => this.#abort(abortError('TimeoutError', 'The operation timed out')), this.#timeoutMs);\n"
    "    }\n"
    "    removeEventListener(type, fn) {\n"
    "        this.#listeners = this.#listeners.filter((l) => l.fn !== fn);\n"
    "        if (!this.#listeners.length && this.#timer) { clearTimeout(this.#timer); this.#timer = null; }\n"
    "    }\n"
    "    #abort(reason) {\n"
    "        if (this.#aborted) return;\n"
    "        this.#aborted = true;\n"
    "        this.#reason = reason === undefined ? abortError('AbortError', 'This operation was aborted') : reason;\n"
    "        if (this.#timer) { clearTimeout(this.#timer); this.#timer = null; }\n"
    "        const ev = { type: 'abort', target: this };\n"
    "        const ls = this.#listeners;\n"
    "        this.#listeners = ls.filter((l) => !l.once);\n"
    "        if (typeof this.onabort === 'function') this.onabort(ev);\n"
    "        for (const l of ls) l.fn.call(this, ev);\n"
    "    }\n"
    "    static abort(reason) { const s = new AbortSignal(); s.#abort(reason); return s; }\n"
    "    static timeout(ms) { const s = new AbortSignal(); s.#timeoutMs = Math.max(1, Number(ms) || 0); return s; }\n"
    "    static { abortVia = (s, reason) => s.#abort(reason); }\n"
    "}\n"
    "class AbortController {\n"
    "    #signal = new AbortSignal();\n"
    "    get signal() { return this.#signal; }\n"
    "    abort(reason) { abortVia(this.#signal, reason); }\n"
    "}\n"
    "def('TextEncoder', TextEncoder);\n"
    "def('TextDecoder', TextDecoder);\n"
    "def('AbortSignal', AbortSignal);\n"
    "def('AbortController', AbortController);\n"
    "}";

static JSValue js_utf8_encode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) {
        return JS_EXCEPTION;
    }
    JSValue r = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)s, len);
    JS_FreeCString(ctx, s);
    return r;
}

/* QuickJS turns bad UTF-8 into U+FFFD silently; fatal: true has to throw instead */
static bool utf8_valid(const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n || (len == 2 && c < 0xc2) || (len == 4 && c > 0xf4)) {
            return false;
        }
        for (int k = 1; k < len; k++) {
            if ((p[i + k] & 0xc0) != 0x80) {
                return false;
            }
        }
        if ((len == 3 && c == 0xe0 && p[i + 1] < 0xa0) || (len == 3 && c == 0xed && p[i + 1] >= 0xa0) ||
            (len == 4 && c == 0xf0 && p[i + 1] < 0x90) || (len == 4 && c == 0xf4 && p[i + 1] >= 0x90)) {
            return false;   /* overlong, surrogate, past U+10FFFF */
        }
        i += len;
    }
    return true;
}

static JSValue js_utf8_decode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    size_t off, len, bpe;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &bpe);
    if (JS_IsException(ab)) {
        return ab;
    }
    size_t ab_len;
    uint8_t *p = JS_GetArrayBuffer(ctx, &ab_len, ab);
    JS_FreeValue(ctx, ab);
    if (!p && len) {
        return JS_EXCEPTION;
    }
    if (JS_ToBool(ctx, argv[1]) && !utf8_valid(p + off, len)) {
        return JS_ThrowTypeError(ctx, "TextDecoder: the data is not valid utf-8");
    }
    return JS_NewStringLen(ctx, (const char *)p + off, len);
}

int qjs_esp_add_web_globals(JSContext *ctx)
{
    JSValue v = JS_Eval(ctx, timers_src, sizeof(timers_src) - 1, "<web timers>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) {
        return -1;
    }
    /* module eval gives a promise; no top-level await in there so it's settled already */
    if (JS_PromiseState(ctx, v) == JS_PROMISE_REJECTED) {
        JS_Throw(ctx, JS_PromiseResult(ctx, v));
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);

    JSValue factory = JS_Eval(ctx, classes_src, sizeof(classes_src) - 1, "<web globals>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(factory)) {
        return -1;
    }
    JSValue args[2] = {
        JS_NewCFunction(ctx, js_utf8_encode, "utf8Encode", 1),
        JS_NewCFunction(ctx, js_utf8_decode, "utf8Decode", 2),
    };
    v = JS_Call(ctx, factory, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, factory);
    if (JS_IsException(v)) {
        return -1;
    }
    JS_FreeValue(ctx, v);
    return 0;
}
