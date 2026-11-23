/*
 * Internal glue shared by the loop and the native modules. Not public API.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include "sdkconfig.h"
#include "qjs_esp.h"

/* not quickjs' list.h: it shares the LIST_H guard with FreeRTOS' list.h */
#define container_of(ptr, type, member) ((type *)((uint8_t *)(ptr) - offsetof(type, member)))

#define QJS_ESP_HAVE_IO (CONFIG_QJS_ESP_MOD_UDP || CONFIG_QJS_ESP_MOD_POSIX_SOCKET)

#define countof(a) (sizeof(a) / sizeof((a)[0]))

/*
 * Something that keeps the loop alive: an open socket, a receiving UDP handle,
 * a connecting/open websocket. While ref'd, the loop holds a reference to the
 * JS object so it can't be GC'd under a pending callback.
 */
typedef struct qjs_esp_handle {
    TAILQ_ENTRY(qjs_esp_handle) link;
    JSValue obj;                                /* JS_UNDEFINED when not ref'd */
    void (*detach)(struct qjs_esp_handle *h);   /* loop teardown: drop callbacks, close */
} qjs_esp_handle_t;

void qjs_esp_handle_init(qjs_esp_handle_t *h, void (*detach)(qjs_esp_handle_t *h));
void qjs_esp_handle_ref(JSContext *ctx, qjs_esp_handle_t *h, JSValueConst obj);
/* May drop the last reference and run the finalizer - don't touch h after. */
void qjs_esp_handle_unref(JSContext *ctx, qjs_esp_handle_t *h);

qjs_esp_loop_t *qjs_esp_loop_get(JSContext *ctx);

/*
 * Socket readiness, the uv_poll_t equivalent. The I/O task select()s on armed
 * watchers and posts hits to the JS task; the callback runs there and the
 * watcher is re-armed afterwards (level triggered). Read/write happens in the
 * JS task, so nothing is shared with the I/O task except the fd number.
 */
enum { QJS_IO_READ = 1, QJS_IO_WRITE = 2 };

typedef struct qjs_esp_io qjs_esp_io_t;
typedef void (*qjs_esp_io_cb_t)(qjs_esp_io_t *w, int revents);

struct qjs_esp_io {
    qjs_esp_io_cb_t cb;
    int fd;
    int8_t slot;        /* -1 = not watching */
    uint8_t events;     /* QJS_IO_* */
    bool armed;         /* owned by the loop, under io_lock */
};

void qjs_esp_io_init(qjs_esp_io_t *w, qjs_esp_io_cb_t cb);
/* Start watching, or change events if already watching. 0 or -errno. */
int qjs_esp_io_start(qjs_esp_loop_t *l, qjs_esp_io_t *w, int fd, int events);
/* Must be called before close(fd). */
void qjs_esp_io_stop(qjs_esp_loop_t *l, qjs_esp_io_t *w);
static inline bool qjs_esp_io_active(const qjs_esp_io_t *w)
{
    return w->slot >= 0;
}

/* Shared receive buffer for the socket modules. JS task only. */
uint8_t *qjs_esp_loop_scratch(qjs_esp_loop_t *l, size_t *len);

/* misc helpers (util.c) */
void *qjs_esp_buf_alloc(size_t size);
void qjs_esp_call(JSContext *ctx, JSValueConst fn, JSValueConst this_val, int argc, JSValueConst *argv);
/* Error with .message = strerror(err), .errno = err */
JSValue qjs_esp_errno_error(JSContext *ctx, int err);
JSValue qjs_esp_throw_errno(JSContext *ctx, int err);
/* ArrayBuffer or typed array -> pointer + length. Valid until the next JS call. */
int qjs_esp_get_bytes(JSContext *ctx, JSValueConst v, uint8_t **p, size_t *len);
/* { ip, port } <-> sockaddr */
int qjs_esp_obj2addr(JSContext *ctx, JSValueConst obj, struct sockaddr_storage *ss, socklen_t *len);
JSValue qjs_esp_addr2obj(JSContext *ctx, const struct sockaddr *sa);
#if CONFIG_LWIP_IPV6
/* "fe80::1%2" -> address + scope id. Leaves the port alone. */
bool qjs_esp_parse_ip6(const char *ip, struct sockaddr_in6 *a6);
#endif
/*
 * Class ids are per runtime in quickjs-ng, so they live in the loop.
 * Finalizers can't rely on the loop still existing; use JS_GetAnyOpaque().
 */
enum { QJS_ESP_CLASS_UDP, QJS_ESP_CLASS_SOCK, QJS_ESP_CLASS_WS, QJS_ESP_CLASS_MAX };
/* Register on first use. 0 = ok, -1 = exception set (e.g. no loop). */
int qjs_esp_class_init(JSContext *ctx, int which, const JSClassDef *def);
JSClassID qjs_esp_class_id(JSContext *ctx, int which);
