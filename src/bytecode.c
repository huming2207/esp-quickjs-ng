/*
 * Bytecode containers: compile on the device, or check and run what a host
 * compiled. Format in qjs_esp.h.
 *
 * QuickJS trusts bytecode completely. The container is checked before
 * JS_ReadObject() sees a byte of it, but that only catches the wrong file or
 * a truncated one, not a hostile one.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "quickjs-libc.h"
#include "qjs_esp_priv.h"

#define FORMAT 1

static void put_hdr(uint8_t *p, uint8_t flags, uint32_t len)
{
    memcpy(p, "QJBC", 4);
    p[4] = FORMAT;
    p[5] = flags;
    p[6] = p[7] = 0;
    p[8] = len;
    p[9] = len >> 8;
    p[10] = len >> 16;
    p[11] = len >> 24;
}

#if CONFIG_QJS_ESP_BC_COMPILE

uint8_t *qjs_esp_bc_compile(JSContext *ctx, const char *src, size_t len, const char *name,
                            int strip, size_t *out_len)
{
    JSValue fn = JS_Eval(ctx, src, len, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(fn)) {
        return NULL;
    }
    int flags = JS_WRITE_OBJ_BYTECODE;
    flags |= (strip & QJS_ESP_BC_STRIP_SOURCE) ? JS_WRITE_OBJ_STRIP_SOURCE : 0;
    flags |= (strip & QJS_ESP_BC_STRIP_DEBUG) ? JS_WRITE_OBJ_STRIP_DEBUG : 0;
    size_t bc_len;
    uint8_t *bc = JS_WriteObject(ctx, &bc_len, fn, flags);
    JS_FreeValue(ctx, fn);
    if (!bc) {
        return NULL;
    }
    uint8_t *out = bc_len <= UINT32_MAX ? js_malloc(ctx, QJS_ESP_BC_HDR_LEN + bc_len) : NULL;
    if (out) {
        put_hdr(out, 0, bc_len);
        memcpy(out + QJS_ESP_BC_HDR_LEN, bc, bc_len);
        *out_len = QJS_ESP_BC_HDR_LEN + bc_len;
    } else {
        JS_ThrowOutOfMemory(ctx);
    }
    js_free(ctx, bc);
    return out;
}

#endif

#if CONFIG_QJS_ESP_BC_LOAD

JSValue qjs_esp_bc_eval(JSContext *ctx, const uint8_t *blob, size_t len)
{
    if (len < QJS_ESP_BC_HDR_LEN || memcmp(blob, "QJBC", 4) || blob[4] != FORMAT || blob[5] || blob[6] || blob[7]) {
        return JS_ThrowTypeError(ctx, "not a bytecode container");
    }
    uint32_t bc_len = blob[8] | blob[9] << 8 | blob[10] << 16 | (uint32_t)blob[11] << 24;
    if (bc_len == 0 || bc_len != len - QJS_ESP_BC_HDR_LEN) {
        return JS_ThrowTypeError(ctx, "bytecode container: bad length");
    }

    JSValue obj = JS_ReadObject(ctx, blob + QJS_ESP_BC_HDR_LEN, bc_len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj)) {
        return obj;     /* e.g. written by another QuickJS version */
    }
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_MODULE) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowTypeError(ctx, "bytecode isn't a module");
    }
    if (JS_ResolveModule(ctx, obj) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (js_module_set_import_meta(ctx, obj, false, true) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return JS_EvalFunction(ctx, obj);   /* frees obj */
}

#endif
