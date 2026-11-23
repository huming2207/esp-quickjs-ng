/*
 * UDP, after txiki.js src/mod_udp.c (MIT, Saúl Ibarra Corretgé).
 *
 * Same JS surface as txiki's native UDP handle, minus libuv:
 *   - lwIP socket + the loop's I/O watcher instead of uv_udp_t
 *   - send() is synchronous (lwIP UDP never waits for buffer space, it fails
 *     with ENOMEM), so there's no send queue and no onsend
 *   - received datagrams land in the loop's shared buffer, then get copied
 *     into a Uint8Array; nothing is allocated per packet on the C side
 *
 * SPDX-License-Identifier: MIT
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <unistd.h>
#include "quickjs-libc.h"
#include "qjs_esp_priv.h"

#if CONFIG_QJS_ESP_MOD_UDP

/* same values as libuv */
#define UDP_IPV6ONLY 1
#define UDP_REUSEADDR 4
#define LEAVE_GROUP 0
#define JOIN_GROUP 1

/* don't hog the JS task when someone floods us */
#define MAX_DGRAMS_PER_WAKEUP 16

typedef struct {
    qjs_esp_handle_t h;     /* ref'd while receiving */
    qjs_esp_io_t io;
    JSContext *ctx;
    int fd;
    int af;                 /* AF_UNSPEC until there's a socket */
    bool bound;
    bool closed;            /* close() is final, nothing brings the socket back */
    JSValue onrecv;
} udp_t;

static udp_t *udp_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, qjs_esp_class_id(ctx, QJS_ESP_CLASS_UDP));
}

/* libuv creates the socket lazily when the family isn't known up front */
static int ensure_sock(JSContext *ctx, udp_t *u, int af)
{
    if (u->fd >= 0) {
        return 0;
    }
    if (u->closed) {
        qjs_esp_throw_errno(ctx, EBADF);
        return -1;
    }
    if (u->af != AF_UNSPEC) {
        af = u->af;
    }
    int fd = socket(af, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        qjs_esp_throw_errno(ctx, errno);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    u->fd = fd;
    u->af = af;
    return 0;
}

static int need_sock(JSContext *ctx, udp_t *u)
{
    if (u->fd < 0) {
        qjs_esp_throw_errno(ctx, EBADF);
        return -1;
    }
    return 0;
}

static void udp_close(udp_t *u)
{
    qjs_esp_io_stop(qjs_esp_loop_get(u->ctx), &u->io);
    if (u->fd >= 0) {
        close(u->fd);
        u->fd = -1;
    }
    u->closed = true;
    qjs_esp_handle_unref(u->ctx, &u->h);    /* last: may free u */
}

static void udp_detach(qjs_esp_handle_t *h)
{
    udp_t *u = container_of(h, udp_t, h);
    JS_FreeValue(u->ctx, u->onrecv);
    u->onrecv = JS_UNDEFINED;
    udp_close(u);
}

static void udp_on_readable(qjs_esp_io_t *w, int revents)
{
    udp_t *u = container_of(w, udp_t, io);
    JSContext *ctx = u->ctx;
    size_t cap;
    uint8_t *buf = qjs_esp_loop_scratch(qjs_esp_loop_get(ctx), &cap);
    /* onrecv may close() us and drop the loop's ref, keep u alive till we're done */
    JSValue self = JS_DupValue(ctx, u->h.obj);

    for (int i = 0; i < MAX_DGRAMS_PER_WAKEUP && qjs_esp_io_active(&u->io); i++) {
        struct sockaddr_storage ss;
        struct iovec iov = { .iov_base = buf, .iov_len = cap };
        struct msghdr m = { .msg_name = &ss, .msg_namelen = sizeof(ss), .msg_iov = &iov, .msg_iovlen = 1 };
        ssize_t n = recvmsg(u->fd, &m, MSG_DONTWAIT);
        JSValue args[2] = { JS_UNDEFINED, JS_UNDEFINED };

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            args[1] = qjs_esp_errno_error(ctx, errno);
        } else {
            JSValue data = JS_NewUint8ArrayCopy(ctx, buf, n);
            JSValue addr = qjs_esp_addr2obj(ctx, (struct sockaddr *)&ss);
            JSValue o = JS_NewObjectProto(ctx, JS_NULL);
            if (JS_IsException(data) || JS_IsException(addr) || JS_IsException(o)) {
                JS_FreeValue(ctx, data);    /* out of memory: drop this one */
                JS_FreeValue(ctx, addr);
                JS_FreeValue(ctx, o);
                js_std_dump_error(ctx);
                break;
            }
            JS_DefinePropertyValueStr(ctx, o, "data", data, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, o, "partial", JS_NewBool(ctx, m.msg_flags & MSG_TRUNC), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, o, "addr", addr, JS_PROP_C_W_E);
            args[0] = o;
        }
        qjs_esp_call(ctx, u->onrecv, self, 2, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        if (n < 0) {
            break;
        }
    }
    JS_FreeValue(ctx, self);
}

static void udp_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSClassID cid;
    udp_t *u = JS_GetAnyOpaque(val, &cid);
    if (!u) {
        return;
    }
    /* receiving => ref'd => can't be finalized, so no watcher to stop here */
    if (u->fd >= 0) {
        close(u->fd);
    }
    JS_FreeValueRT(rt, u->onrecv);
    js_free_rt(rt, u);
}

static void udp_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID cid;
    udp_t *u = JS_GetAnyOpaque(val, &cid);
    if (u) {
        JS_MarkValue(rt, u->onrecv, mark_func);
    }
}

static const JSClassDef udp_class = {
    "UDP",
    .finalizer = udp_finalizer,
    .gc_mark = udp_mark,
};

static JSValue udp_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    int32_t af = AF_UNSPEC;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &af, argv[0])) {
        return JS_EXCEPTION;
    }
    JSValue obj = qjs_esp_new_obj(ctx, new_target, QJS_ESP_CLASS_UDP);
    if (JS_IsException(obj)) {
        return obj;
    }
    udp_t *u = js_mallocz(ctx, sizeof(*u));
    if (!u) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    qjs_esp_handle_init(&u->h, udp_detach);
    qjs_esp_io_init(&u->io, udp_on_readable);
    u->ctx = ctx;
    u->fd = -1;
    u->af = AF_UNSPEC;
    u->onrecv = JS_UNDEFINED;
    JS_SetOpaque(obj, u);

    if (af != AF_UNSPEC && ensure_sock(ctx, u, af)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue udp_onrecv_get(JSContext *ctx, JSValueConst this_val)
{
    udp_t *u = udp_get(ctx, this_val);
    return u ? JS_DupValue(ctx, u->onrecv) : JS_EXCEPTION;
}

static JSValue udp_onrecv_set(JSContext *ctx, JSValueConst this_val, JSValueConst v)
{
    udp_t *u = udp_get(ctx, this_val);
    if (!u) {
        return JS_EXCEPTION;
    }
    if (JS_IsFunction(ctx, v) || JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, u->onrecv);
        u->onrecv = JS_DupValue(ctx, v);
    }
    return JS_UNDEFINED;
}

static JSValue udp_close_js(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    udp_t *u = udp_get(ctx, this_val);
    if (!u) {
        return JS_EXCEPTION;
    }
    udp_close(u);   /* this_val holds a ref, u survives the unref */
    return JS_UNDEFINED;
}

static JSValue udp_bind(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    udp_t *u = udp_get(ctx, this_val);
    struct sockaddr_storage ss;
    socklen_t len;
    int32_t flags = 0;

    if (!u || qjs_esp_obj2addr(ctx, argv[0], &ss, &len)) {
        return JS_EXCEPTION;
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToInt32(ctx, &flags, argv[1])) {
        return JS_EXCEPTION;
    }
    if (ensure_sock(ctx, u, ss.ss_family)) {
        return JS_EXCEPTION;
    }
    int on = 1;
    if ((flags & UDP_REUSEADDR) && setsockopt(u->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on))) {
        return qjs_esp_throw_errno(ctx, errno);
    }
#if CONFIG_LWIP_IPV6
    if ((flags & UDP_IPV6ONLY) && setsockopt(u->fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on))) {
        return qjs_esp_throw_errno(ctx, errno);
    }
#endif
    if (bind(u->fd, (struct sockaddr *)&ss, len)) {
        return qjs_esp_throw_errno(ctx, errno);
    }
    u->bound = true;
    return JS_UNDEFINED;
}

static JSValue udp_connect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    udp_t *u = udp_get(ctx, this_val);
    struct sockaddr_storage ss;
    socklen_t len;

    if (!u || qjs_esp_obj2addr(ctx, argv[0], &ss, &len) || ensure_sock(ctx, u, ss.ss_family)) {
        return JS_EXCEPTION;
    }
    if (connect(u->fd, (struct sockaddr *)&ss, len)) {
        return qjs_esp_throw_errno(ctx, errno);
    }
    u->bound = true;    /* lwIP picks a local port on connect */
    return JS_UNDEFINED;
}

/* send(data[, addr]) -> bytes sent */
static JSValue udp_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    udp_t *u = udp_get(ctx, this_val);
    struct sockaddr_storage ss;
    socklen_t len = 0;
    uint8_t *p;
    size_t n;

    if (!u) {
        return JS_EXCEPTION;
    }
    /* address first: parsing it can run JS, which could detach the buffer */
    bool to = argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]);
    if (to && (qjs_esp_obj2addr(ctx, argv[1], &ss, &len) || ensure_sock(ctx, u, ss.ss_family))) {
        return JS_EXCEPTION;
    }
    if (need_sock(ctx, u) || qjs_esp_get_bytes(ctx, argv[0], &p, &n)) {
        return JS_EXCEPTION;
    }
    ssize_t r = sendto(u->fd, p, n, 0, to ? (struct sockaddr *)&ss : NULL, len);
    if (r < 0) {
        return qjs_esp_throw_errno(ctx, errno);
    }
    u->bound = true;
    return JS_NewInt64(ctx, r);
}

static JSValue udp_start_recv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    udp_t *u = udp_get(ctx, this_val);
    if (!u) {
        return JS_EXCEPTION;
    }
    /* like libuv: receiving on an unbound socket binds it to any:0 */
    if (!u->bound) {
        if (ensure_sock(ctx, u, AF_INET)) {
            return JS_EXCEPTION;
        }
        struct sockaddr_storage ss = { .ss_family = u->af };    /* zeroes = any:0 */
        socklen_t len = sizeof(struct sockaddr_in);
#if CONFIG_LWIP_IPV6
        if (u->af == AF_INET6) {
            len = sizeof(struct sockaddr_in6);
        }
#endif
        if (bind(u->fd, (struct sockaddr *)&ss, len)) {
            return qjs_esp_throw_errno(ctx, errno);
        }
        u->bound = true;
    }
    if (need_sock(ctx, u)) {
        return JS_EXCEPTION;
    }
    int r = qjs_esp_io_start(qjs_esp_loop_get(ctx), &u->io, u->fd, QJS_IO_READ);
    if (r) {
        return qjs_esp_throw_errno(ctx, -r);
    }
    qjs_esp_handle_ref(ctx, &u->h, this_val);
    return JS_UNDEFINED;
}

static JSValue udp_stop_recv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    udp_t *u = udp_get(ctx, this_val);
    if (!u) {
        return JS_EXCEPTION;
    }
    qjs_esp_io_stop(qjs_esp_loop_get(ctx), &u->io);
    qjs_esp_handle_unref(ctx, &u->h);
    return JS_UNDEFINED;
}

static JSValue udp_fileno(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    udp_t *u = udp_get(ctx, this_val);
    if (!u || need_sock(ctx, u)) {
        return JS_EXCEPTION;
    }
    return JS_NewInt32(ctx, u->fd);
}

static JSValue udp_getname(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int peer)
{
    udp_t *u = udp_get(ctx, this_val);
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);

    if (!u || need_sock(ctx, u)) {
        return JS_EXCEPTION;
    }
    int r = peer ? getpeername(u->fd, (struct sockaddr *)&ss, &len)
                 : getsockname(u->fd, (struct sockaddr *)&ss, &len);
    if (r) {
        return qjs_esp_throw_errno(ctx, errno);
    }
    return qjs_esp_addr2obj(ctx, (struct sockaddr *)&ss);
}

/* setMembership(group, ifaceAddr | null, JOIN_GROUP | LEAVE_GROUP) */
static JSValue udp_set_membership(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    udp_t *u = udp_get(ctx, this_val);
    int32_t join;
    if (!u || need_sock(ctx, u) || JS_ToInt32(ctx, &join, argv[2])) {
        return JS_EXCEPTION;
    }
    const char *group = JS_ToCString(ctx, argv[0]);
    if (!group) {
        return JS_EXCEPTION;
    }
    const char *iface = NULL;
    if (!JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) && !(iface = JS_ToCString(ctx, argv[1]))) {
        JS_FreeCString(ctx, group);
        return JS_EXCEPTION;
    }
    int r = -1, err = EINVAL;

    struct ip_mreq m4 = { .imr_interface.s_addr = htonl(INADDR_ANY) };
    if (inet_pton(AF_INET, group, &m4.imr_multiaddr) == 1) {
        if (!iface || inet_pton(AF_INET, iface, &m4.imr_interface) == 1) {
            r = setsockopt(u->fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &m4, sizeof(m4));
            err = errno;
        }
    }
#if CONFIG_LWIP_IPV6 && LWIP_IPV6_MLD
    else {
        struct sockaddr_in6 g6, i6 = { 0 };
        if (qjs_esp_parse_ip6(group, &g6) && (!iface || qjs_esp_parse_ip6(iface, &i6))) {
            struct ipv6_mreq m6 = { .ipv6mr_multiaddr = g6.sin6_addr, .ipv6mr_interface = i6.sin6_scope_id };
            r = setsockopt(u->fd, IPPROTO_IPV6, join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP, &m6, sizeof(m6));
            err = errno;
        }
    }
#endif
    JS_FreeCString(ctx, group);
    JS_FreeCString(ctx, iface);
    return r ? qjs_esp_throw_errno(ctx, err) : JS_UNDEFINED;
}

/* option setters: v4/v6 flavours of the same knob */
enum { OPT_BROADCAST, OPT_MCAST_LOOP, OPT_MCAST_TTL };

static JSValue udp_setopt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int which)
{
    udp_t *u = udp_get(ctx, this_val);
    int32_t v;
    if (!u || need_sock(ctx, u)) {
        return JS_EXCEPTION;
    }
    if (which == OPT_MCAST_TTL) {
        if (JS_ToInt32(ctx, &v, argv[0])) {
            return JS_EXCEPTION;
        }
        if (v < 1 || v > 255) {
            return JS_ThrowRangeError(ctx, "ttl must be 1..255");
        }
    } else {
        v = JS_ToBool(ctx, argv[0]);
    }

    int r;
    uint8_t v8 = v;     /* lwIP wants u8_t for the multicast knobs, v4 and v6 */
    if (which == OPT_BROADCAST) {
        r = setsockopt(u->fd, SOL_SOCKET, SO_BROADCAST, &v, sizeof(v));
    } else if (u->af == AF_INET) {
        int opt = which == OPT_MCAST_TTL ? IP_MULTICAST_TTL : IP_MULTICAST_LOOP;
        r = setsockopt(u->fd, IPPROTO_IP, opt, &v8, sizeof(v8));
    } else {
#if CONFIG_LWIP_IPV6
        int opt = which == OPT_MCAST_TTL ? IPV6_MULTICAST_HOPS : IPV6_MULTICAST_LOOP;
        r = setsockopt(u->fd, IPPROTO_IPV6, opt, &v8, sizeof(v8));
#else
        r = -1;
        errno = EAFNOSUPPORT;
#endif
    }
    return r ? qjs_esp_throw_errno(ctx, errno) : JS_UNDEFINED;
}

/* setMulticastInterface("192.168.1.10" | "::%2") */
static JSValue udp_set_mcast_iface(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    udp_t *u = udp_get(ctx, this_val);
    if (!u || need_sock(ctx, u)) {
        return JS_EXCEPTION;
    }
    const char *iface = JS_ToCString(ctx, argv[0]);
    if (!iface) {
        return JS_EXCEPTION;
    }
    int r = -1, err = EINVAL;
    struct in_addr a4;
    if (u->af == AF_INET && inet_pton(AF_INET, iface, &a4) == 1) {
        r = setsockopt(u->fd, IPPROTO_IP, IP_MULTICAST_IF, &a4, sizeof(a4));
        err = errno;
    }
#if CONFIG_LWIP_IPV6
    struct sockaddr_in6 a6;
    if (u->af == AF_INET6 && qjs_esp_parse_ip6(iface, &a6)) {
        uint8_t idx = a6.sin6_scope_id;    /* lwIP netif indexes are u8_t */
        r = setsockopt(u->fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx, sizeof(idx));
        err = errno;
    }
#endif
    JS_FreeCString(ctx, iface);
    return r ? qjs_esp_throw_errno(ctx, err) : JS_UNDEFINED;
}

static const JSCFunctionListEntry udp_proto[] = {
    JS_CGETSET_DEF("onrecv", udp_onrecv_get, udp_onrecv_set),
    JS_CFUNC_DEF("close", 0, udp_close_js),
    JS_CFUNC_DEF("startRecv", 0, udp_start_recv),
    JS_CFUNC_DEF("stopRecv", 0, udp_stop_recv),
    JS_CFUNC_DEF("send", 2, udp_send),
    JS_CFUNC_DEF("fileno", 0, udp_fileno),
    JS_CFUNC_MAGIC_DEF("getsockname", 0, udp_getname, 0),
    JS_CFUNC_MAGIC_DEF("getpeername", 0, udp_getname, 1),
    JS_CFUNC_DEF("connect", 1, udp_connect),
    JS_CFUNC_DEF("bind", 2, udp_bind),
    JS_CFUNC_DEF("setMembership", 3, udp_set_membership),
    JS_CFUNC_MAGIC_DEF("setBroadcast", 1, udp_setopt, OPT_BROADCAST),
    JS_CFUNC_MAGIC_DEF("setMulticastLoop", 1, udp_setopt, OPT_MCAST_LOOP),
    JS_CFUNC_MAGIC_DEF("setMulticastTTL", 1, udp_setopt, OPT_MCAST_TTL),
    JS_CFUNC_DEF("setMulticastInterface", 1, udp_set_mcast_iface),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "UDP", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry udp_consts[] = {
    JS_PROP_INT32_DEF("UDP_IPV6ONLY", UDP_IPV6ONLY, 0),
    JS_PROP_INT32_DEF("UDP_REUSEADDR", UDP_REUSEADDR, 0),
    JS_PROP_INT32_DEF("JOIN_GROUP", JOIN_GROUP, 0),
    JS_PROP_INT32_DEF("LEAVE_GROUP", LEAVE_GROUP, 0),
    JS_PROP_INT32_DEF("AF_INET", AF_INET, 0),
    JS_PROP_INT32_DEF("AF_INET6", AF_INET6, 0),
};

static int udp_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (qjs_esp_class_init(ctx, QJS_ESP_CLASS_UDP, &udp_class)) {
        return -1;
    }
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, udp_proto, countof(udp_proto));
    JSValue ctor = JS_NewCFunction2(ctx, udp_ctor, "UDP", 1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, qjs_esp_class_id(ctx, QJS_ESP_CLASS_UDP), proto);
    JS_SetModuleExport(ctx, m, "UDP", ctor);
    return JS_SetModuleExportList(ctx, m, udp_consts, countof(udp_consts));
}

JSModuleDef *qjs_esp_init_module_udp(JSContext *ctx, const char *name)
{
    JSModuleDef *m = JS_NewCModule(ctx, name, udp_module_init);
    if (m) {
        JS_AddModuleExport(ctx, m, "UDP");
        JS_AddModuleExportList(ctx, m, udp_consts, countof(udp_consts));
    }
    return m;
}

#endif /* CONFIG_QJS_ESP_MOD_UDP */
