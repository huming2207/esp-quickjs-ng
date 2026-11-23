/*
 * Raw BSD sockets, after txiki.js src/mod_posix-socket.c (MIT).
 *
 * Same JS surface as txiki's posixSocketLoad() namespace, on lwIP:
 *   - poll() goes through the loop's I/O task instead of uv_poll_t
 *   - sockaddrs / option values / cmsgs from JS are copied to aligned stack
 *     buffers first: a Uint8Array can start at any byte, lwIP reads them as
 *     structs, and xtensa faults on unaligned loads
 *   - reads go through the loop's shared buffer, so read(n) may return less
 *     than n (it may anyway, it's a socket)
 *   - DISCONNECT is detected by peeking for EOF; PRIORITIZED (OOB) doesn't
 *     exist in lwIP and is never reported
 *   - setNonBlocking() added: without it connect()/accept() block the JS task
 *
 * SPDX-License-Identifier: MIT
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <string.h>
#include <unistd.h>
#include "qjs_esp_priv.h"

#if CONFIG_QJS_ESP_MOD_POSIX_SOCKET

#define MAX_IOV 8
#define MAX_OPT 64      /* optval / cmsg stack buffer */

/* uv_poll_event bits */
enum { EV_READABLE = 1, EV_WRITABLE = 2, EV_DISCONNECT = 4, EV_PRIORITIZED = 8 };

typedef struct {
    qjs_esp_handle_t h;     /* ref'd while polling */
    qjs_esp_io_t io;
    JSContext *ctx;
    int fd;                 /* -1 once closed */
    bool stream;            /* SOCK_STREAM: 0 bytes means EOF, not an empty datagram */
    int poll_events;        /* EV_* as asked for */
    JSValue poll_cb;
} psock_t;

static psock_t *psock_get(JSContext *ctx, JSValueConst obj)
{
    psock_t *s = JS_GetOpaque2(ctx, obj, qjs_esp_class_id(ctx, QJS_ESP_CLASS_SOCK));
    if (s && s->fd < 0) {
        JS_ThrowTypeError(ctx, "Socket closed");
        return NULL;
    }
    return s;
}

static JSValue throw_errno(JSContext *ctx)
{
    return qjs_esp_throw_errno(ctx, errno);
}

/* Uint8Array holding a sockaddr -> aligned copy */
static int get_sockaddr(JSContext *ctx, JSValueConst v, struct sockaddr_storage *ss, socklen_t *len)
{
    uint8_t *p;
    size_t n;
    if (qjs_esp_get_bytes(ctx, v, &p, &n)) {
        return -1;
    }
    if (n > sizeof(*ss)) {
        JS_ThrowRangeError(ctx, "sockaddr too long");
        return -1;
    }
    memcpy(ss, p, n);
    *len = n;
    return 0;
}

static void psock_stop_poll(psock_t *s)
{
    qjs_esp_io_stop(qjs_esp_loop_get(s->ctx), &s->io);
    JS_FreeValue(s->ctx, s->poll_cb);
    s->poll_cb = JS_UNDEFINED;
    qjs_esp_handle_unref(s->ctx, &s->h);    /* may free s */
}

static void psock_detach(qjs_esp_handle_t *h)
{
    psock_t *s = container_of(h, psock_t, h);
    JS_FreeValue(s->ctx, s->poll_cb);
    s->poll_cb = JS_UNDEFINED;
    qjs_esp_io_stop(qjs_esp_loop_get(s->ctx), &s->io);
    close(s->fd);
    s->fd = -1;
    qjs_esp_handle_unref(s->ctx, &s->h);
}

static void psock_on_ready(qjs_esp_io_t *w, int revents)
{
    psock_t *s = container_of(w, psock_t, io);
    JSContext *ctx = s->ctx;
    int ev = revents & (EV_READABLE | EV_WRITABLE);     /* same bits as QJS_IO_* */

    /* lwIP has no POLLRDHUP: a closed peer shows up as readable EOF, a reset
     * one as a read error. poll() only allows DISCONNECT together with
     * READABLE, so the level-triggered watch can't spin on unread data. */
    int status = 0;
    uint8_t b;
    if ((s->poll_events & EV_DISCONNECT) && (revents & QJS_IO_READ)) {
        ssize_t r = recv(s->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r == 0 && s->stream) {
            ev |= EV_DISCONNECT;
        } else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            status = -errno;   /* like uv_poll_cb */
            ev |= EV_DISCONNECT;
        }
    }
    ev &= s->poll_events;
    if (!ev) {
        return;
    }
    JSValue self = JS_DupValue(ctx, s->h.obj);  /* callback may pollStop()/close() */
    JSValue args[2] = { JS_NewInt32(ctx, status), JS_NewInt32(ctx, ev) };
    qjs_esp_call(ctx, s->poll_cb, self, 2, args);
    JS_FreeValue(ctx, self);
}

static void psock_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSClassID cid;
    psock_t *s = JS_GetAnyOpaque(val, &cid);
    if (!s) {
        return;
    }
    if (s->fd >= 0) {
        close(s->fd);   /* polling => ref'd => can't get here, no watcher to stop */
    }
    JS_FreeValueRT(rt, s->poll_cb);
    js_free_rt(rt, s);
}

static void psock_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID cid;
    psock_t *s = JS_GetAnyOpaque(val, &cid);
    if (s) {
        JS_MarkValue(rt, s->poll_cb, mark_func);
    }
}

static const JSClassDef psock_class = {
    "PosixSocket",
    .finalizer = psock_finalizer,
    .gc_mark = psock_mark,
};

/* takes ownership of fd, closes it on failure */
static JSValue psock_wrap(JSContext *ctx, JSValueConst new_target, int fd)
{
    int type = 0;
    socklen_t tl = sizeof(type);
    getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl);
    JSValue obj = qjs_esp_new_obj(ctx, new_target, QJS_ESP_CLASS_SOCK);
    psock_t *s = JS_IsException(obj) ? NULL : js_mallocz(ctx, sizeof(*s));
    if (!s) {
        close(fd);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    qjs_esp_handle_init(&s->h, psock_detach);
    qjs_esp_io_init(&s->io, psock_on_ready);
    s->ctx = ctx;
    s->fd = fd;
    s->stream = type == SOCK_STREAM;
    s->poll_cb = JS_UNDEFINED;
    JS_SetOpaque(obj, s);
    return obj;
}

static JSValue psock_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    uint32_t domain, type, proto;
    if (JS_ToUint32(ctx, &domain, argv[0]) || JS_ToUint32(ctx, &type, argv[1]) ||
        JS_ToUint32(ctx, &proto, argv[2])) {
        return JS_EXCEPTION;
    }
    int fd = socket(domain, type, proto);
    return fd < 0 ? throw_errno(ctx) : psock_wrap(ctx, new_target, fd);
}

static JSValue psock_from_fd(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    int32_t fd;
    if (JS_ToInt32(ctx, &fd, argv[0])) {
        return JS_EXCEPTION;
    }
    /* sockets only: not a file, not the loop's eventfd */
    int type;
    socklen_t tl = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) < 0) {
        return JS_ThrowTypeError(ctx, "%d is not a socket: %s", (int)fd, strerror(errno));
    }
    return psock_wrap(ctx, JS_UNDEFINED, fd);
}

static JSValue psock_bind_connect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int is_connect)
{
    psock_t *s = psock_get(ctx, this_val);
    struct sockaddr_storage ss;
    socklen_t len;
    if (!s || get_sockaddr(ctx, argv[0], &ss, &len)) {
        return JS_EXCEPTION;
    }
    int r = is_connect ? connect(s->fd, (struct sockaddr *)&ss, len) : bind(s->fd, (struct sockaddr *)&ss, len);
    return r ? throw_errno(ctx) : JS_UNDEFINED;
}

static JSValue psock_accept(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    psock_t *s = psock_get(ctx, this_val);
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (!s) {
        return JS_EXCEPTION;
    }
    int fd = accept(s->fd, (struct sockaddr *)&ss, &len);
    if (fd < 0) {
        return throw_errno(ctx);
    }
    JSValue obj = psock_wrap(ctx, JS_UNDEFINED, fd);
    if (JS_IsException(obj)) {
        return obj;     /* fd closed by psock_wrap() */
    }
    JSValue sa = JS_NewUint8ArrayCopy(ctx, (uint8_t *)&ss, len);
    if (JS_IsException(sa) || JS_SetPropertyStr(ctx, obj, "_sockaddr", sa) < 0) {
        JS_FreeValue(ctx, obj);     /* closes fd */
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue psock_listen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    psock_t *s = psock_get(ctx, this_val);
    int32_t backlog;
    if (!s || JS_ToInt32(ctx, &backlog, argv[0])) {
        return JS_EXCEPTION;
    }
    return listen(s->fd, backlog) ? throw_errno(ctx) : JS_UNDEFINED;
}

static JSValue psock_shutdown(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    psock_t *s = psock_get(ctx, this_val);
    int32_t how;
    if (!s || JS_ToInt32(ctx, &how, argv[0])) {
        return JS_EXCEPTION;
    }
    return shutdown(s->fd, how) ? throw_errno(ctx) : JS_UNDEFINED;
}

static JSValue psock_set_nonblocking(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    psock_t *s = psock_get(ctx, this_val);
    if (!s) {
        return JS_EXCEPTION;
    }
    int fl = fcntl(s->fd, F_GETFL);
    if (fl < 0) {
        return throw_errno(ctx);
    }
    fl = JS_ToBool(ctx, argv[0]) ? fl | O_NONBLOCK : fl & ~O_NONBLOCK;
    return fcntl(s->fd, F_SETFL, fl) < 0 ? throw_errno(ctx) : JS_UNDEFINED;
}

static JSValue psock_setopt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    psock_t *s = psock_get(ctx, this_val);
    int32_t level, name;
    uint8_t *p;
    size_t n;
    _Alignas(8) uint8_t opt[MAX_OPT];

    if (!s || JS_ToInt32(ctx, &level, argv[0]) || JS_ToInt32(ctx, &name, argv[1]) ||
        qjs_esp_get_bytes(ctx, argv[2], &p, &n)) {
        return JS_EXCEPTION;
    }
    if (n > sizeof(opt)) {
        return JS_ThrowRangeError(ctx, "option value too long");
    }
    memcpy(opt, p, n);
    return setsockopt(s->fd, level, name, opt, n) ? throw_errno(ctx) : JS_UNDEFINED;
}

/* getopt(level, name[, len]) -> Uint8Array */
static JSValue psock_getopt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    psock_t *s = psock_get(ctx, this_val);
    int32_t level, name;
    uint32_t want = MAX_OPT;
    _Alignas(8) uint8_t opt[MAX_OPT];

    if (!s || JS_ToInt32(ctx, &level, argv[0]) || JS_ToInt32(ctx, &name, argv[1])) {
        return JS_EXCEPTION;
    }
    if (argc > 2 && !JS_IsUndefined(argv[2]) && JS_ToUint32(ctx, &want, argv[2])) {
        return JS_EXCEPTION;
    }
    socklen_t len = want < sizeof(opt) ? want : sizeof(opt);
    if (getsockopt(s->fd, level, name, opt, &len)) {
        return throw_errno(ctx);
    }
    return JS_NewUint8ArrayCopy(ctx, opt, len);
}

/* read(n) / recv(n, flags): Uint8Array, or null on EOF */
static JSValue psock_recv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int with_flags)
{
    psock_t *s = psock_get(ctx, this_val);
    uint32_t want;
    int32_t flags = 0;
    size_t cap;

    if (!s || JS_ToUint32(ctx, &want, argv[0]) || (with_flags && JS_ToInt32(ctx, &flags, argv[1]))) {
        return JS_EXCEPTION;
    }
    uint8_t *buf = qjs_esp_loop_scratch(qjs_esp_loop_get(ctx), &cap);
    ssize_t r = recv(s->fd, buf, want < cap ? want : cap, flags);
    if (r < 0) {
        return throw_errno(ctx);
    }
    return r == 0 && s->stream ? JS_NULL : JS_NewUint8ArrayCopy(ctx, buf, r);
}

static JSValue psock_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    psock_t *s = psock_get(ctx, this_val);
    uint8_t *p;
    size_t n;
    if (!s || qjs_esp_get_bytes(ctx, argv[0], &p, &n)) {
        return JS_EXCEPTION;
    }
    ssize_t r = write(s->fd, p, n);
    return r < 0 ? throw_errno(ctx) : JS_NewInt64(ctx, r);
}

/* recvmsg(bufsz[, controlsz]) -> { addr, data, control? } */
static JSValue psock_recvmsg(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    psock_t *s = psock_get(ctx, this_val);
    uint32_t want, ctrl_want = 0;
    size_t cap;
    struct sockaddr_storage ss;
    _Alignas(8) uint8_t ctrl[MAX_OPT];

    if (!s || JS_ToUint32(ctx, &want, argv[0])) {
        return JS_EXCEPTION;
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToUint32(ctx, &ctrl_want, argv[1])) {
        return JS_EXCEPTION;
    }
    uint8_t *buf = qjs_esp_loop_scratch(qjs_esp_loop_get(ctx), &cap);
    struct iovec iov = { .iov_base = buf, .iov_len = want < cap ? want : cap };
    struct msghdr m = {
        .msg_name = &ss, .msg_namelen = sizeof(ss), .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ctrl_want ? ctrl : NULL, .msg_controllen = ctrl_want < sizeof(ctrl) ? ctrl_want : sizeof(ctrl),
    };
    ssize_t r = recvmsg(s->fd, &m, 0);
    if (r < 0) {
        return throw_errno(ctx);
    }
    JSValue ret = JS_NewObject(ctx);
    JSValue addr = JS_NewUint8ArrayCopy(ctx, (uint8_t *)&ss, m.msg_namelen);
    JSValue data = JS_NewUint8ArrayCopy(ctx, buf, r);
    JSValue cm = ctrl_want ? JS_NewUint8ArrayCopy(ctx, ctrl, m.msg_controllen) : JS_UNDEFINED;
    if (JS_IsException(ret) || JS_IsException(addr) || JS_IsException(data) || JS_IsException(cm)) {
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, addr);
        JS_FreeValue(ctx, data);
        JS_FreeValue(ctx, cm);
        return JS_EXCEPTION;    /* the datagram is gone, nothing to do about that */
    }
    JS_SetPropertyStr(ctx, ret, "addr", addr);
    if (ctrl_want) {
        JS_SetPropertyStr(ctx, ret, "control", cm);
    }
    JS_SetPropertyStr(ctx, ret, "data", data);
    return ret;
}

/* sendmsg(addr | undefined, control | undefined, flags, ...data) -> bytes sent */
static JSValue psock_sendmsg(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    psock_t *s = psock_get(ctx, this_val);
    struct sockaddr_storage ss;
    socklen_t ss_len = 0;
    _Alignas(8) uint8_t ctrl[MAX_OPT];
    size_t ctrl_len = 0;
    struct iovec iov[MAX_IOV];
    int32_t flags;

    if (!s) {
        return JS_EXCEPTION;
    }
    if (argc < 4) {
        return JS_ThrowTypeError(ctx, "expected at least 4 arguments");
    }
    if (argc - 3 > MAX_IOV) {
        return JS_ThrowRangeError(ctx, "at most %d buffers", MAX_IOV);
    }
    if (JS_ToInt32(ctx, &flags, argv[2])) {
        return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]) && get_sockaddr(ctx, argv[0], &ss, &ss_len)) {
        return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        uint8_t *p;
        if (qjs_esp_get_bytes(ctx, argv[1], &p, &ctrl_len)) {
            return JS_EXCEPTION;
        }
        if (ctrl_len > sizeof(ctrl)) {
            return JS_ThrowRangeError(ctx, "control data too long");
        }
        memcpy(ctrl, p, ctrl_len);
    }
    /* last, so nothing can run JS and detach these before sendmsg() */
    for (int i = 3; i < argc; i++) {
        uint8_t *p;
        size_t n;
        if (qjs_esp_get_bytes(ctx, argv[i], &p, &n)) {
            return JS_EXCEPTION;
        }
        iov[i - 3] = (struct iovec){ .iov_base = p, .iov_len = n };
    }
    struct msghdr m = {
        .msg_name = ss_len ? &ss : NULL, .msg_namelen = ss_len,
        .msg_control = ctrl_len ? ctrl : NULL, .msg_controllen = ctrl_len,
        .msg_iov = iov, .msg_iovlen = argc - 3,
    };
    ssize_t r = sendmsg(s->fd, &m, flags);
    return r < 0 ? throw_errno(ctx) : JS_NewInt64(ctx, r);
}

/* poll(events, cb): cb.call(sock, status, events), until pollStop()/close() */
static JSValue psock_poll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    psock_t *s = psock_get(ctx, this_val);
    uint32_t events;
    if (!s || JS_ToUint32(ctx, &events, argv[0])) {
        return JS_EXCEPTION;
    }
    if (!events || events > 0xf) {
        return JS_ThrowRangeError(ctx, "invalid poll events");
    }
    if (!JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "callback must be a function");
    }
    /* lwIP has no OOB data, and no way to wait for EOF without also waking
     * on data: DISCONNECT needs READABLE, PRIORITIZED alone is useless */
    if ((events & EV_DISCONNECT) && !(events & EV_READABLE)) {
        return qjs_esp_throw_errno(ctx, ENOTSUP);
    }
    int io = events & (EV_READABLE | EV_WRITABLE);
    if (!io) {
        return qjs_esp_throw_errno(ctx, ENOTSUP);
    }
    int r = qjs_esp_io_start(qjs_esp_loop_get(ctx), &s->io, s->fd, io);
    if (r) {
        return qjs_esp_throw_errno(ctx, -r);
    }
    s->poll_events = events;
    JS_FreeValue(ctx, s->poll_cb);
    s->poll_cb = JS_DupValue(ctx, argv[1]);
    qjs_esp_handle_ref(ctx, &s->h, this_val);
    return JS_UNDEFINED;
}

static JSValue psock_poll_stop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    psock_t *s = psock_get(ctx, this_val);
    if (!s) {
        return JS_EXCEPTION;
    }
    psock_stop_poll(s);
    return JS_UNDEFINED;
}

static JSValue psock_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    psock_t *s = psock_get(ctx, this_val);
    if (!s) {
        return JS_EXCEPTION;
    }
    psock_stop_poll(s);     /* this_val keeps s alive across the unref */
    close(s->fd);
    s->fd = -1;
    return JS_UNDEFINED;
}

static JSValue psock_polling(JSContext *ctx, JSValueConst this_val)
{
    psock_t *s = psock_get(ctx, this_val);
    return s ? JS_NewBool(ctx, qjs_esp_io_active(&s->io)) : JS_EXCEPTION;
}

static JSValue psock_fileno(JSContext *ctx, JSValueConst this_val)
{
    psock_t *s = psock_get(ctx, this_val);
    return s ? JS_NewInt32(ctx, s->fd) : JS_EXCEPTION;
}

/* lwIP only answers SO_TYPE; no SO_DOMAIN / SO_PROTOCOL */
static JSValue psock_info(JSContext *ctx, JSValueConst this_val)
{
    psock_t *s = psock_get(ctx, this_val);
    int v;
    socklen_t len = sizeof(v);
    if (!s) {
        return JS_EXCEPTION;
    }
    JSValue info = JS_NewObject(ctx);
    if (getsockopt(s->fd, SOL_SOCKET, SO_TYPE, &v, &len) == 0) {
        JSValue si = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, si, "type", JS_NewInt32(ctx, v));
        JS_SetPropertyStr(ctx, info, "socket", si);
    }
    return info;
}

static const JSCFunctionListEntry psock_proto[] = {
    JS_CFUNC_MAGIC_DEF("bind", 1, psock_bind_connect, 0),
    JS_CFUNC_MAGIC_DEF("connect", 1, psock_bind_connect, 1),
    JS_CFUNC_DEF("close", 0, psock_close),
    JS_CFUNC_DEF("accept", 0, psock_accept),
    JS_CFUNC_DEF("setopt", 3, psock_setopt),
    JS_CFUNC_DEF("getopt", 3, psock_getopt),
    JS_CFUNC_DEF("listen", 1, psock_listen),
    JS_CFUNC_MAGIC_DEF("read", 1, psock_recv, 0),
    JS_CFUNC_DEF("write", 1, psock_write),
    JS_CFUNC_DEF("shutdown", 1, psock_shutdown),
    JS_CFUNC_MAGIC_DEF("recv", 2, psock_recv, 1),
    JS_CFUNC_DEF("sendmsg", 4, psock_sendmsg),
    JS_CFUNC_DEF("recvmsg", 2, psock_recvmsg),
    JS_CFUNC_DEF("poll", 2, psock_poll),
    JS_CFUNC_DEF("pollStop", 0, psock_poll_stop),
    JS_CFUNC_DEF("setNonBlocking", 1, psock_set_nonblocking),
    JS_CGETSET_DEF("polling", psock_polling, NULL),
    JS_CGETSET_DEF("fileno", psock_fileno, NULL),
    JS_CGETSET_DEF("info", psock_info, NULL),
};

/* ---- namespace helpers ---- */

static JSValue ns_sockaddr_inet(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    struct sockaddr_storage ss;
    socklen_t len;
    if (qjs_esp_obj2addr(ctx, argv[0], &ss, &len)) {
        return JS_EXCEPTION;
    }
    return JS_NewUint8ArrayCopy(ctx, (uint8_t *)&ss, len);
}

/* takes uv-style negative codes too */
static JSValue ns_strerror(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    int32_t e;
    if (JS_ToInt32(ctx, &e, argv[0])) {
        return JS_EXCEPTION;
    }
    return JS_NewString(ctx, strerror(e < 0 ? -e : e));
}

static JSValue ns_if_nametoindex(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_EXCEPTION;
    }
    unsigned idx = if_nametoindex(name);
    JS_FreeCString(ctx, name);
    return idx ? JS_NewUint32(ctx, idx) : qjs_esp_throw_errno(ctx, ENXIO);
}

static JSValue ns_if_indextoname(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    uint32_t idx;
    char name[IF_NAMESIZE];
    if (JS_ToUint32(ctx, &idx, argv[0])) {
        return JS_EXCEPTION;
    }
    return if_indextoname(idx, name) ? JS_NewString(ctx, name) : qjs_esp_throw_errno(ctx, ENXIO);
}

/* RFC 1071 internet checksum, result in network byte order like txiki's */
static JSValue ns_checksum(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    uint8_t *p;
    size_t n;
    if (qjs_esp_get_bytes(ctx, argv[0], &p, &n)) {
        return JS_EXCEPTION;
    }
    /* same seed as txiki, so same results. Differs from a 0 seed only when
     * the sum is zero: 0x0000 instead of 0xffff. Mind that for UDP, where a
     * zero checksum on the wire means "none" (RFC 768: send 0xffff instead) */
    uint64_t acc = 0xffff;  /* 64 bits: no lost carries on big buffers */
    for (size_t i = 0; i + 1 < n; i += 2) {
        acc += (uint32_t)p[i] << 8 | p[i + 1];
    }
    if (n & 1) {
        acc += (uint32_t)p[n - 1] << 8;
    }
    while (acc >> 16) {
        acc = (acc & 0xffff) + (acc >> 16);
    }
    return JS_NewInt32(ctx, htons((uint16_t)~acc));
}

#define DEF(x) JS_PROP_INT32_DEF(#x, x, JS_PROP_ENUMERABLE)

/* only what lwIP actually has */
static const JSCFunctionListEntry defines[] = {
    DEF(AF_INET), DEF(AF_INET6),
    DEF(SOCK_STREAM), DEF(SOCK_DGRAM), DEF(SOCK_RAW),
    DEF(SOL_SOCKET),
    DEF(SO_REUSEADDR), DEF(SO_KEEPALIVE), DEF(SO_LINGER), DEF(SO_BROADCAST),
    DEF(SO_RCVBUF), DEF(SO_RCVTIMEO), DEF(SO_SNDTIMEO),     /* no SO_SNDBUF in lwIP */
    DEF(SO_ERROR), DEF(SO_TYPE), DEF(SO_BINDTODEVICE),
    DEF(IPPROTO_IP), DEF(IPPROTO_IPV6), DEF(IPPROTO_ICMP), DEF(IPPROTO_TCP), DEF(IPPROTO_UDP),
    DEF(TCP_NODELAY), DEF(TCP_KEEPIDLE), DEF(TCP_KEEPINTVL), DEF(TCP_KEEPCNT),
    DEF(IP_TTL), DEF(IP_TOS),
    DEF(MSG_PEEK), DEF(MSG_DONTWAIT), DEF(MSG_WAITALL), DEF(MSG_MORE),
    DEF(SHUT_RD), DEF(SHUT_WR), DEF(SHUT_RDWR),
    /* errno values differ per libc, don't make JS hardcode them */
    DEF(EAGAIN), DEF(EWOULDBLOCK), DEF(EINPROGRESS), DEF(EALREADY), DEF(EISCONN),
    DEF(ECONNREFUSED), DEF(ECONNRESET), DEF(ETIMEDOUT), DEF(ENOTCONN),
};

static const JSCFunctionListEntry poll_bits[] = {
    JS_PROP_INT32_DEF("READABLE", EV_READABLE, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("WRITABLE", EV_WRITABLE, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("DISCONNECT", EV_DISCONNECT, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("PRIORITIZED", EV_PRIORITIZED, JS_PROP_ENUMERABLE),
};

/* names kept from txiki so its posix-socket.js wrapper works as-is */
static const JSCFunctionListEntry ns_exports[] = {
    JS_CFUNC_DEF("create_sockaddr_inet", 1, ns_sockaddr_inet),
    JS_CFUNC_DEF("uv_strerror", 1, ns_strerror),
    JS_CFUNC_DEF("strerror", 1, ns_strerror),
    JS_PROP_INT32_DEF("sizeof_struct_sockaddr", sizeof(struct sockaddr), 0),
    JS_OBJECT_DEF("defines", defines, countof(defines), JS_PROP_C_W_E),
    JS_OBJECT_DEF("uv_poll_event_bits", poll_bits, countof(poll_bits), JS_PROP_C_W_E),
    JS_CFUNC_DEF("socket_from_fd", 1, psock_from_fd),
    JS_CFUNC_DEF("if_nametoindex", 1, ns_if_nametoindex),
    JS_CFUNC_DEF("if_indextoname", 1, ns_if_indextoname),
    JS_CFUNC_DEF("checksum", 1, ns_checksum),
};

static int psock_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (qjs_esp_class_init(ctx, QJS_ESP_CLASS_SOCK, &psock_class)) {
        return -1;
    }
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, psock_proto, countof(psock_proto));
    JSValue ctor = JS_NewCFunction2(ctx, psock_ctor, "PosixSocket", 3, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, qjs_esp_class_id(ctx, QJS_ESP_CLASS_SOCK), JS_DupValue(ctx, proto));
    JS_SetModuleExport(ctx, m, "PosixSocketProto", proto);
    JS_SetModuleExport(ctx, m, "PosixSocket", ctor);
    return JS_SetModuleExportList(ctx, m, ns_exports, countof(ns_exports));
}

JSModuleDef *qjs_esp_init_module_posix_socket(JSContext *ctx, const char *name)
{
    JSModuleDef *m = JS_NewCModule(ctx, name, psock_module_init);
    if (m) {
        JS_AddModuleExport(ctx, m, "PosixSocket");
        JS_AddModuleExport(ctx, m, "PosixSocketProto");
        JS_AddModuleExportList(ctx, m, ns_exports, countof(ns_exports));
    }
    return m;
}

#endif /* CONFIG_QJS_ESP_MOD_POSIX_SOCKET */
