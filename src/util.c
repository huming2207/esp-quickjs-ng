/*
 * Small helpers shared by the native modules.
 *
 * SPDX-License-Identifier: MIT
 */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "quickjs-libc.h"
#include "qjs_esp_priv.h"

void *qjs_esp_buf_alloc(size_t size)
{
#if CONFIG_SPIRAM
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
#else
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
#endif
}

void qjs_esp_call(JSContext *ctx, JSValueConst fn, JSValueConst this_val, int argc, JSValueConst *argv)
{
    /* building an argument ran out of memory: report it, don't hand JS a JS_EXCEPTION */
    for (int i = 0; i < argc; i++) {
        if (JS_IsException(argv[i])) {
            js_std_dump_error(ctx);
            return;
        }
    }
    if (!JS_IsFunction(ctx, fn)) {
        return;
    }
    /* the handler may replace itself (onrecv = null) and drop the last ref */
    JSValue f = JS_DupValue(ctx, fn);
    JSValue ret = JS_Call(ctx, f, this_val, argc, argv);
    JS_FreeValue(ctx, f);
    if (JS_IsException(ret)) {
        js_std_dump_error(ctx);
    }
    JS_FreeValue(ctx, ret);
}

JSValue qjs_esp_errno_error(JSContext *ctx, int err)
{
    JSValue e = JS_NewError(ctx);
    if (JS_IsException(e)) {
        return e;
    }
    JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, strerror(err)),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, e, "errno", JS_NewInt32(ctx, err), JS_PROP_C_W_E);
    return e;
}

JSValue qjs_esp_throw_errno(JSContext *ctx, int err)
{
    JSValue e = qjs_esp_errno_error(ctx, err);
    return JS_IsException(e) ? e : JS_Throw(ctx, e);
}

/* QuickJS has no C API for DataView: go through its buffer/byteOffset/byteLength.
 * Anything faking those can only point inside a real ArrayBuffer, we bound-check. */
static int view_bytes(JSContext *ctx, JSValueConst v, uint8_t **p, size_t *len)
{
    uint64_t off = 0, n = 0;
    size_t ab_len;
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "expected an ArrayBuffer, typed array or DataView");
        return -1;
    }
    /* one at a time: stop at the first getter that throws */
    JSValue ab = JS_GetPropertyStr(ctx, v, "buffer");
    JSValue o = JS_IsException(ab) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, v, "byteOffset");
    JSValue l = JS_IsException(o) || JS_IsException(ab) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, v, "byteLength");
    int bad = JS_HasException(ctx) || !JS_IsArrayBuffer(ab) || JS_ToIndex(ctx, &off, o) || JS_ToIndex(ctx, &n, l);
    uint8_t *base = bad ? NULL : JS_GetArrayBuffer(ctx, &ab_len, ab);  /* last, after the getters */
    JS_FreeValue(ctx, ab);
    JS_FreeValue(ctx, o);
    JS_FreeValue(ctx, l);
    if (bad || JS_HasException(ctx) || off > ab_len || n > ab_len - off) {
        if (!JS_HasException(ctx)) {
            JS_ThrowTypeError(ctx, "expected an ArrayBuffer, typed array or DataView");
        }
        return -1;
    }
    *p = base ? base + off : NULL;
    *len = n;
    return 0;
}

int qjs_esp_get_bytes(JSContext *ctx, JSValueConst v, uint8_t **p, size_t *len)
{
    static uint8_t empty;
    int r;

    *p = NULL;
    *len = 0;
    if (JS_IsArrayBuffer(v)) {
        *p = JS_GetArrayBuffer(ctx, len, v);
        r = JS_HasException(ctx) ? -1 : 0;
    } else if (JS_GetTypedArrayType(v) >= 0) {
        size_t off, n, abuf_len;
        JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &off, &n, NULL);
        if (JS_IsException(abuf)) {
            return -1;
        }
        uint8_t *base = JS_GetArrayBuffer(ctx, &abuf_len, abuf);
        JS_FreeValue(ctx, abuf);    /* still owned by the typed array */
        r = JS_HasException(ctx) ? -1 : 0;
        *p = base ? base + off : NULL;
        *len = n;
    } else {
        r = view_bytes(ctx, v, p, len);
    }
    if (r == 0 && !*p) {
        *p = &empty;    /* 0 bytes; memcpy() & co. must not see NULL */
        *len = 0;
    }
    return r;
}

#if CONFIG_LWIP_IPV6
/* "fe80::1%2": lwIP's inet_pton doesn't do scope ids, peel it off */
bool qjs_esp_parse_ip6(const char *ip, struct sockaddr_in6 *a6)
{
    char buf[INET6_ADDRSTRLEN];
    const char *pct = strchr(ip, '%');
    size_t n = pct ? (size_t)(pct - ip) : strlen(ip);

    if (n >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, ip, n);
    buf[n] = '\0';
    if (inet_pton(AF_INET6, buf, &a6->sin6_addr) != 1) {
        return false;
    }
    a6->sin6_family = AF_INET6;
    a6->sin6_scope_id = 0;
    if (pct) {
        /* "%2" or "%st1" (interface name) */
        char *end = (char *)pct + 1;
        unsigned long id = 0;
        if (*end >= '0' && *end <= '9') {   /* strtoul would also take "-1" and " 2" */
            id = strtoul(pct + 1, &end, 10);
        }
        if (end == pct + 1 || *end) {
            id = if_nametoindex(pct + 1);
        }
        if (!id) {
            return false;
        }
        a6->sin6_scope_id = id;
    }
    return true;
}
#endif

int qjs_esp_obj2addr(JSContext *ctx, JSValueConst obj, struct sockaddr_storage *ss, socklen_t *len)
{
    double d = 0;
    JSValue v = JS_GetPropertyStr(ctx, obj, "port");
    int r = JS_IsUndefined(v) ? 0 : JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    if (r) {
        return -1;
    }
    /* not ToUint32: that would turn 65536 + 80 into port 80 */
    if (!(d >= 0 && d <= 65535) || d != (int)d) {
        JS_ThrowRangeError(ctx, "invalid port");
        return -1;
    }
    uint16_t port = d;

    v = JS_GetPropertyStr(ctx, obj, "ip");
    const char *ip = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!ip) {
        return -1;
    }

    memset(ss, 0, sizeof(*ss));
    struct sockaddr_in *a4 = (struct sockaddr_in *)ss;
    r = 0;
    if (inet_pton(AF_INET, ip, &a4->sin_addr) == 1) {
        a4->sin_family = AF_INET;
        a4->sin_port = htons(port);
        *len = sizeof(*a4);
#if CONFIG_LWIP_IPV6
    } else if (qjs_esp_parse_ip6(ip, (struct sockaddr_in6 *)ss)) {
        ((struct sockaddr_in6 *)ss)->sin6_port = htons(port);
        *len = sizeof(struct sockaddr_in6);
#endif
    } else {
        JS_ThrowTypeError(ctx, "invalid IP address: %s", ip);
        r = -1;
    }
    JS_FreeCString(ctx, ip);
    return r;
}

JSValue qjs_esp_addr2obj(JSContext *ctx, const struct sockaddr *sa)
{
    char buf[INET6_ADDRSTRLEN];
    JSValue obj = JS_NewObjectProto(ctx, JS_NULL);
    if (JS_IsException(obj)) {
        return obj;
    }

    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *a4 = (const struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &a4->sin_addr, buf, sizeof(buf));
        JS_DefinePropertyValueStr(ctx, obj, "family", JS_NewInt32(ctx, 4), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "ip", JS_NewString(ctx, buf), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "port", JS_NewInt32(ctx, ntohs(a4->sin_port)), JS_PROP_C_W_E);
    }
#if CONFIG_LWIP_IPV6
    else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)sa;
        inet_ntop(AF_INET6, &a6->sin6_addr, buf, sizeof(buf));
        JS_DefinePropertyValueStr(ctx, obj, "family", JS_NewInt32(ctx, 6), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "ip", JS_NewString(ctx, buf), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "port", JS_NewInt32(ctx, ntohs(a6->sin6_port)), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "flowInfo", JS_NewUint32(ctx, ntohl(a6->sin6_flowinfo)), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "scopeId", JS_NewUint32(ctx, a6->sin6_scope_id), JS_PROP_C_W_E);
    }
#endif
    return obj;
}

JSValue qjs_esp_new_obj(JSContext *ctx, JSValueConst new_target, int which)
{
    JSClassID cid = qjs_esp_class_id(ctx, which);
    if (JS_IsUndefined(new_target)) {
        return JS_NewObjectClass(ctx, cid);
    }
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) {
        return proto;
    }
    if (!JS_IsObject(proto)) {      /* GetPrototypeFromConstructor: fall back to ours */
        JS_FreeValue(ctx, proto);
        return JS_NewObjectClass(ctx, cid);
    }
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, cid);
    JS_FreeValue(ctx, proto);
    return obj;
}
