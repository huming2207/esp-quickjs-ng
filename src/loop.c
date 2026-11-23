/*
 * Event loop on FreeRTOS.
 *
 * The JS task sleeps on a queue. Three kinds of things post to it:
 *   - the I/O task, when a watched socket is ready (EV_IO)
 *   - websocket tasks and app code via qjs_esp_loop_post() (EV_WORK)
 *   - the I/O task once more on shutdown (EV_IO_DONE)
 * Timers and promise jobs stay in quickjs-libc; js_std_loop_once() runs them
 * and tells us how long we may sleep.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <sys/select.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_eventfd.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "quickjs-libc.h"
#include "qjs_esp_priv.h"

static const char *TAG = "qjs_loop";

enum { EV_WORK, EV_IO, EV_IO_DONE };

typedef struct {
    uint8_t kind;
    uint8_t revents;    /* EV_IO */
    uint8_t slot;       /* EV_IO */
    uint16_t gen;       /* EV_IO: drops events for a watcher that went away */
    qjs_esp_work_cb_t cb;
    void *arg;
} ev_t;

#define MAX_REJECTIONS 4

struct qjs_esp_loop {
    JSContext *ctx;
    QueueHandle_t q;
    TAILQ_HEAD(, qjs_esp_handle) handles;   /* ref'd handles; the loop runs while non-empty */
    JSClassID class_ids[QJS_ESP_CLASS_MAX];
    int n_rej;
    struct {
        JSValue promise;
        JSValue reason;
    } rej[MAX_REJECTIONS];
#if QJS_ESP_HAVE_IO
    SemaphoreHandle_t io_lock;  /* slots[] and every watcher's armed/events */
    TaskHandle_t io_task;
    int io_wake;                /* eventfd, kicks the I/O task out of select() */
    volatile bool io_quit;
    struct {
        qjs_esp_io_t *w;
        uint16_t gen;
    } slots[CONFIG_QJS_ESP_IO_MAX_WATCHERS];
    uint8_t *scratch;
#endif
};

qjs_esp_loop_t *qjs_esp_loop_get(JSContext *ctx)
{
    return JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
}

int qjs_esp_class_init(JSContext *ctx, int which, const JSClassDef *def)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    qjs_esp_loop_t *l = JS_GetRuntimeOpaque(rt);

    if (!l) {
        JS_ThrowInternalError(ctx, "no event loop, call qjs_esp_loop_new() first");
        return -1;
    }
    if (l->class_ids[which]) {
        return 0;
    }
    JS_NewClassID(rt, &l->class_ids[which]);
    if (JS_NewClass(rt, l->class_ids[which], def)) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

JSClassID qjs_esp_class_id(JSContext *ctx, int which)
{
    qjs_esp_loop_t *l = qjs_esp_loop_get(ctx);
    return l ? l->class_ids[which] : 0;
}

/* ---- handles ---- */

void qjs_esp_handle_init(qjs_esp_handle_t *h, void (*detach)(qjs_esp_handle_t *h))
{
    h->obj = JS_UNDEFINED;
    h->detach = detach;
}

void qjs_esp_handle_ref(JSContext *ctx, qjs_esp_handle_t *h, JSValueConst obj)
{
    if (!JS_IsUndefined(h->obj)) {
        return;
    }
    h->obj = JS_DupValue(ctx, obj);
    TAILQ_INSERT_TAIL(&qjs_esp_loop_get(ctx)->handles, h, link);
}

void qjs_esp_handle_unref(JSContext *ctx, qjs_esp_handle_t *h)
{
    if (JS_IsUndefined(h->obj)) {
        return;
    }
    JSValue obj = h->obj;
    TAILQ_REMOVE(&qjs_esp_loop_get(ctx)->handles, h, link);
    h->obj = JS_UNDEFINED;
    JS_FreeValue(ctx, obj);
}

/* ---- unhandled rejections ----
 * Same idea as quickjs-libc's tracker, but print instead of exit(1), and a
 * fixed table instead of a malloc'd list. If the table is full we just
 * report straight away. */

static void report_rejection(JSContext *ctx, JSValueConst reason)
{
    fprintf(stderr, "Possibly unhandled promise rejection: ");
    JS_Throw(ctx, JS_DupValue(ctx, reason));
    js_std_dump_error(ctx);
}

static void on_rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                         bool is_handled, void *opaque)
{
    qjs_esp_loop_t *l = opaque;

    if (is_handled) {
        for (int i = 0; i < l->n_rej; i++) {
            if (JS_IsStrictEqual(ctx, l->rej[i].promise, promise)) {
                JS_FreeValue(ctx, l->rej[i].promise);
                JS_FreeValue(ctx, l->rej[i].reason);
                l->rej[i] = l->rej[--l->n_rej];
                break;
            }
        }
        return;
    }
    if (l->n_rej == MAX_REJECTIONS) {
        report_rejection(ctx, reason);
        return;
    }
    l->rej[l->n_rej].promise = JS_DupValue(ctx, promise);
    l->rej[l->n_rej].reason = JS_DupValue(ctx, reason);
    l->n_rej++;
}

static void flush_rejections(qjs_esp_loop_t *l, bool report)
{
    while (l->n_rej > 0) {
        l->n_rej--;
        if (report) {
            report_rejection(l->ctx, l->rej[l->n_rej].reason);
        }
        JS_FreeValue(l->ctx, l->rej[l->n_rej].promise);
        JS_FreeValue(l->ctx, l->rej[l->n_rej].reason);
    }
}

/* ---- socket watchers + I/O task ---- */

#if QJS_ESP_HAVE_IO

static void io_wake(qjs_esp_loop_t *l)
{
    uint64_t one = 1;
    if (write(l->io_wake, &one, sizeof(one)) != sizeof(one)) {
        ESP_LOGW(TAG, "eventfd write: %d", errno);
    }
}

void qjs_esp_io_init(qjs_esp_io_t *w, qjs_esp_io_cb_t cb)
{
    w->cb = cb;
    w->fd = -1;
    w->slot = -1;
    w->events = 0;
    w->armed = false;
}

int qjs_esp_io_start(qjs_esp_loop_t *l, qjs_esp_io_t *w, int fd, int events)
{
    xSemaphoreTake(l->io_lock, portMAX_DELAY);
    if (w->slot < 0) {
        int i = 0;
        while (i < countof(l->slots) && l->slots[i].w) {
            i++;
        }
        if (i == countof(l->slots)) {
            xSemaphoreGive(l->io_lock);
            return -EMFILE;     /* bump CONFIG_QJS_ESP_IO_MAX_WATCHERS */
        }
        l->slots[i].w = w;
        w->slot = i;
    }
    w->fd = fd;
    w->events = events;
    w->armed = true;
    xSemaphoreGive(l->io_lock);
    io_wake(l);
    return 0;
}

void qjs_esp_io_stop(qjs_esp_loop_t *l, qjs_esp_io_t *w)
{
    if (w->slot < 0) {
        return;
    }
    xSemaphoreTake(l->io_lock, portMAX_DELAY);
    l->slots[w->slot].w = NULL;
    l->slots[w->slot].gen++;    /* anything already queued for this slot is now stale */
    w->slot = -1;
    w->armed = false;
    xSemaphoreGive(l->io_lock);
    io_wake(l);
}

uint8_t *qjs_esp_loop_scratch(qjs_esp_loop_t *l, size_t *len)
{
    *len = CONFIG_QJS_ESP_RX_BUF_SIZE;
    return l->scratch;
}

static void io_task(void *arg)
{
    qjs_esp_loop_t *l = arg;
    ev_t evs[CONFIG_QJS_ESP_IO_MAX_WATCHERS];

    while (!l->io_quit) {
        fd_set rd, wr, ex;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_ZERO(&ex);
        FD_SET(l->io_wake, &rd);
        int maxfd = l->io_wake;

        xSemaphoreTake(l->io_lock, portMAX_DELAY);
        for (int i = 0; i < countof(l->slots); i++) {
            qjs_esp_io_t *w = l->slots[i].w;
            if (!w || !w->armed) {
                continue;
            }
            if (w->events & QJS_IO_READ) {
                FD_SET(w->fd, &rd);
            }
            if (w->events & QJS_IO_WRITE) {
                FD_SET(w->fd, &wr);
            }
            FD_SET(w->fd, &ex);
            maxfd = w->fd > maxfd ? w->fd : maxfd;
        }
        xSemaphoreGive(l->io_lock);

        if (select(maxfd + 1, &rd, &wr, &ex, NULL) < 0) {
            /* EBADF: JS closed a socket between our snapshot and select().
             * It's unwatched already, the next snapshot won't have it. */
            if (errno != EBADF && errno != EINTR) {
                ESP_LOGW(TAG, "select: %d", errno);
            }
            vTaskDelay(1);
            continue;
        }
        if (FD_ISSET(l->io_wake, &rd)) {
            uint64_t v;
            (void)read(l->io_wake, &v, sizeof(v));
        }

        /* Disarm what fired, then post outside the lock: the queue may be full
         * and the JS task needs the lock to drain it. */
        int n = 0;
        xSemaphoreTake(l->io_lock, portMAX_DELAY);
        for (int i = 0; i < countof(l->slots); i++) {
            qjs_esp_io_t *w = l->slots[i].w;
            if (!w || !w->armed) {
                continue;
            }
            int rev = 0;
            if (FD_ISSET(w->fd, &rd)) {
                rev |= QJS_IO_READ;
            }
            if (FD_ISSET(w->fd, &wr)) {
                rev |= QJS_IO_WRITE;
            }
            if (FD_ISSET(w->fd, &ex)) {
                rev |= w->events;   /* error: wake whoever's waiting, their read/write will see it */
            }
            if (rev) {
                w->armed = false;
                evs[n++] = (ev_t){ .kind = EV_IO, .revents = rev, .slot = i, .gen = l->slots[i].gen };
            }
        }
        xSemaphoreGive(l->io_lock);

        for (int i = 0; i < n; i++) {
            xQueueSend(l->q, &evs[i], portMAX_DELAY);
        }
    }

    ev_t done = { .kind = EV_IO_DONE };
    xQueueSend(l->q, &done, portMAX_DELAY);
    vTaskDelete(NULL);
}

static void io_dispatch(qjs_esp_loop_t *l, const ev_t *ev)
{
    xSemaphoreTake(l->io_lock, portMAX_DELAY);
    qjs_esp_io_t *w = l->slots[ev->slot].gen == ev->gen ? l->slots[ev->slot].w : NULL;
    xSemaphoreGive(l->io_lock);
    if (!w) {
        return;
    }

    w->cb(w, ev->revents);

    /* the callback may have stopped (and freed) w - only trust the slot table */
    bool rearm = false;
    xSemaphoreTake(l->io_lock, portMAX_DELAY);
    if (l->slots[ev->slot].gen == ev->gen && l->slots[ev->slot].w == w && !w->armed) {
        w->armed = rearm = true;
    }
    xSemaphoreGive(l->io_lock);
    if (rearm) {
        io_wake(l);
    }
}

static esp_err_t io_init(qjs_esp_loop_t *l)
{
    l->scratch = qjs_esp_buf_alloc(CONFIG_QJS_ESP_RX_BUF_SIZE);
    l->io_lock = xSemaphoreCreateMutex();
    if (!l->scratch || !l->io_lock) {
        return ESP_ERR_NO_MEM;
    }

    /* someone else may have registered it already, that's fine */
    esp_vfs_eventfd_config_t cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
    esp_err_t err = esp_vfs_eventfd_register(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    l->io_wake = eventfd(0, 0);
    if (l->io_wake < 0) {
        return ESP_FAIL;
    }
    if (xTaskCreate(io_task, "qjs_io", CONFIG_QJS_ESP_IO_TASK_STACK, l,
                    CONFIG_QJS_ESP_IO_TASK_PRIO, &l->io_task) != pdPASS) {
        l->io_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void io_deinit(qjs_esp_loop_t *l)
{
    if (l->io_task) {
        l->io_quit = true;
        io_wake(l);
        /* wait for EV_IO_DONE; anything else in the queue is stale by now */
        ev_t ev;
        do {
            xQueueReceive(l->q, &ev, portMAX_DELAY);
        } while (ev.kind != EV_IO_DONE);
    }
    if (l->io_wake >= 0) {
        close(l->io_wake);
    }
    if (l->io_lock) {
        vSemaphoreDelete(l->io_lock);
    }
    heap_caps_free(l->scratch);
}

#endif /* QJS_ESP_HAVE_IO */

/* ---- loop ---- */

static void dispatch(qjs_esp_loop_t *l, const ev_t *ev)
{
    switch (ev->kind) {
    case EV_WORK:
        ev->cb(l->ctx, ev->arg);
        break;
#if QJS_ESP_HAVE_IO
    case EV_IO:
        io_dispatch(l, ev);
        break;
#endif
    default:
        break;
    }
}

esp_err_t qjs_esp_loop_post(qjs_esp_loop_t *l, qjs_esp_work_cb_t cb, void *arg, TickType_t wait)
{
    ev_t ev = { .kind = EV_WORK, .cb = cb, .arg = arg };
    return xQueueSend(l->q, &ev, wait) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void loop_free(qjs_esp_loop_t *l)
{
#if QJS_ESP_HAVE_IO
    io_deinit(l);
#endif
    if (l->q) {
        vQueueDelete(l->q);
    }
    free(l);
}

qjs_esp_loop_t *qjs_esp_loop_new(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    if (JS_GetRuntimeOpaque(rt)) {
        ESP_LOGE(TAG, "runtime already has a loop (or someone else owns the opaque)");
        return NULL;
    }
    qjs_esp_loop_t *l = calloc(1, sizeof(*l));
    if (!l) {
        return NULL;
    }
    l->ctx = ctx;
    TAILQ_INIT(&l->handles);
#if QJS_ESP_HAVE_IO
    l->io_wake = -1;
#endif
    l->q = xQueueCreate(CONFIG_QJS_ESP_EVENT_QUEUE_LEN, sizeof(ev_t));
    if (!l->q) {
        loop_free(l);
        return NULL;
    }
#if QJS_ESP_HAVE_IO
    esp_err_t err = io_init(l);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I/O task init failed: %s", esp_err_to_name(err));
        loop_free(l);
        return NULL;
    }
#endif
    JS_SetRuntimeOpaque(rt, l);
    JS_SetHostPromiseRejectionTracker(rt, on_rejection, l);
    return l;
}

/* round up, or a 5 ms timer on a 10 ms tick spins until it fires */
static TickType_t ms_to_ticks(int ms)
{
    return ((TickType_t)ms * configTICK_RATE_HZ + 999) / 1000;
}

void qjs_esp_loop_run(qjs_esp_loop_t *l)
{
    JSContext *ctx = l->ctx;

    for (;;) {
        int delay = js_std_loop_once(ctx);
        if (delay == -2) {
            js_std_dump_error(ctx);     /* a job or timer threw; keep going */
            continue;
        }
        if (!JS_IsJobPending(JS_GetRuntime(ctx))) {
            flush_rejections(l, true);  /* a .catch() might still be queued otherwise */
        }

        TickType_t wait;
        if (delay >= 0) {
            wait = ms_to_ticks(delay);
        } else if (TAILQ_EMPTY(&l->handles) && uxQueueMessagesWaiting(l->q) == 0) {
            return;     /* no timers, no jobs, nothing open */
        } else {
            wait = portMAX_DELAY;
        }

        ev_t ev;
        if (xQueueReceive(l->q, &ev, wait) == pdTRUE) {
            dispatch(l, &ev);
        }
    }
}

void qjs_esp_loop_free(qjs_esp_loop_t *l)
{
    JSRuntime *rt = JS_GetRuntime(l->ctx);

    /* Detach everything. Sockets close on the spot; websockets have a task
     * to stop and unref themselves from its final event, so keep draining. */
    qjs_esp_handle_t *h, *tmp;
    TAILQ_FOREACH_SAFE(h, &l->handles, link, tmp) {
        h->detach(h);
    }
    while (!TAILQ_EMPTY(&l->handles)) {
        ev_t ev;
        if (xQueueReceive(l->q, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) {
            dispatch(l, &ev);
        } else {
            ESP_LOGW(TAG, "still waiting for handles to close");
        }
    }
    flush_rejections(l, false);
    JS_SetHostPromiseRejectionTracker(rt, NULL, NULL);
    JS_SetRuntimeOpaque(rt, NULL);
    loop_free(l);
}
