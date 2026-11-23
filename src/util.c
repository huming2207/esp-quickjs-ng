/*
 * Small helpers shared by the native modules.
 *
 * SPDX-License-Identifier: MIT
 */

#include <arpa/inet.h>
#include <errno.h>
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

int qjs_esp_get_bytes(JSContext *ctx, JSValueConst v, uint8_t **p, size_t *len)
{
    if (JS_IsArrayBuffer(v)) {
        *p = JS_GetArrayBuffer(ctx, len, v);
        return (*p || *len == 0) && !JS_HasException(ctx) ? 0 : -1;
    }

    size_t off, n, abuf_len;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &off, &n, NULL);
    if (JS_IsException(abuf)) {
        return -1;
    }
    uint8_t *base = JS_GetArrayBuffer(ctx, &abuf_len, abuf);
    JS_FreeValue(ctx, abuf);    /* still owned by the typed array */
    if (!base && JS_HasException(ctx)) {
        return -1;
    }
    *p = base + off;
    *len = n;
    return 0;
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
    a6->sin6_scope_id = pct ? strtoul(pct + 1, NULL, 10) : 0;
    return true;
}
#endif

int qjs_esp_obj2addr(JSContext *ctx, JSValueConst obj, struct sockaddr_storage *ss, socklen_t *len)
{
    uint32_t port = 0;
    JSValue v = JS_GetPropertyStr(ctx, obj, "port");
    int r = JS_IsUndefined(v) ? 0 : JS_ToUint32(ctx, &port, v);
    JS_FreeValue(ctx, v);
    if (r) {
        return -1;
    }
    if (port > 65535) {
        JS_ThrowRangeError(ctx, "invalid port");
        return -1;
    }

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

