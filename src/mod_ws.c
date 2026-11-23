/*
 * WebSocket client, after txiki.js src/ws.c (MIT, Saúl Ibarra Corretgé).
 *
 * Same JS surface as txiki's native WebSocket (onopen/onmessage/onerror/
 * onclose, sendText, sendBinary, close), on esp_transport instead of
 * libwebsockets:
 *
 *   - each connection gets a task: esp_transport is blocking (DNS, TLS
 *     handshake, frame reads), so it lives there, not in the JS task.
 *     The task posts open / message / closed to the loop and is deleted by
 *     the JS task when the closed event is handled.
 *   - one mutex around the transport: the task reads and answers pings,
 *     the JS task sends. mbedTLS can't do both at once.
 *   - sends are synchronous, from the JS task, through a fixed tx buffer
 *     (big messages are fragmented); no queue, so bufferedAmount is always 0.
 *   - messages are reassembled into a fixed rx buffer; the task waits until
 *     the JS task has copied it out (task notification) before reading on.
 *   - no extensions (permessage-deflate etc.) - esp_transport has none.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "http_parser.h"
#include "qjs_esp_priv.h"

#if CONFIG_QJS_ESP_MOD_WS

#define RX_CAP CONFIG_QJS_ESP_WS_MAX_MESSAGE
#define TX_CAP CONFIG_QJS_ESP_WS_TX_CHUNK
#define IO_TIMEOUT_MS CONFIG_QJS_ESP_WS_IO_TIMEOUT_MS
#define POLL_MS 1000    /* how often the task checks for abort / close timeout */

#if CONFIG_QJS_ESP_WS_TASK_STACK_PSRAM
#define TASK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define TASK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

enum { CB_CLOSE, CB_ERROR, CB_MESSAGE, CB_OPEN, CB_MAX };
enum { ST_CONNECTING, ST_OPEN, ST_CLOSING, ST_CLOSED };

typedef struct {
    qjs_esp_handle_t h;         /* ref'd from new WebSocket() until the closed event */
    JSContext *ctx;
    qjs_esp_loop_t *loop;
    JSValue cb[CB_MAX];

    esp_transport_handle_t tcp; /* tcp or ssl */
    esp_transport_handle_t ws;
    SemaphoreHandle_t lock;     /* around every transport call after connect */
    TaskHandle_t task;

    bool dead;                  /* transport closed, under lock */
    volatile uint8_t state;     /* written by the JS task only */
    volatile bool abort;        /* JS -> task: give up now */
    TickType_t close_deadline;  /* ST_CLOSING: stop waiting for the peer's close */

    /* task side, handed to JS through the queue */
    char host[256];
    int port;
    char protocol[64];
    char err[48];
    uint16_t close_code;
    char close_reason[124];     /* RFC 6455: 123 bytes + NUL */

    /* frame / message state, task only */
    int frame_op;
    int frame_left;
    bool frame_fin;
    bool in_msg;
    int msg_op;
    size_t rx_len;
    uint8_t *rx;                /* RX_CAP */
    uint8_t *tx;                /* TX_CAP, JS task only */
} ws_t;

static ws_t *ws_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, qjs_esp_class_id(ctx, QJS_ESP_CLASS_WS));
}

static int ws_write(ws_t *w, int op, const void *p, size_t n)
{
    if (xSemaphoreTake(w->lock, pdMS_TO_TICKS(IO_TIMEOUT_MS)) != pdTRUE) {
        return -1;
    }
    int r = w->dead ? -1 : esp_transport_ws_send_raw(w->ws, op, p, n, IO_TIMEOUT_MS);
    xSemaphoreGive(w->lock);
    return r == (int)n ? 0 : -1;
}

/* ---- JS-task side of the events the connection task posts ---- */

static void ev_open(JSContext *ctx, void *arg)
{
    ws_t *w = arg;
    if (w->state != ST_CONNECTING) {
        return;     /* close() got there first, the task is bailing out */
    }
    w->state = ST_OPEN;
    JSValue p = JS_NewString(ctx, w->protocol);
    qjs_esp_call(ctx, w->cb[CB_OPEN], w->h.obj, 1, &p);
    JS_FreeValue(ctx, p);
}

static void ev_message(JSContext *ctx, void *arg)
{
    ws_t *w = arg;
    JSValue v = w->msg_op == WS_TRANSPORT_OPCODES_TEXT
                ? JS_NewStringLen(ctx, (const char *)w->rx, w->rx_len)
                : JS_NewArrayBufferCopy(ctx, w->rx, w->rx_len);
    xTaskNotifyGive(w->task);   /* copied out, the task may reuse rx */
    qjs_esp_call(ctx, w->cb[CB_MESSAGE], w->h.obj, 1, &v);
    JS_FreeValue(ctx, v);
}

static void ws_release(ws_t *w)
{
    if (w->ws) {
        esp_transport_destroy(w->ws);   /* doesn't take the parent with it */
        w->ws = NULL;
    }
    if (w->tcp) {
        esp_transport_destroy(w->tcp);
        w->tcp = NULL;
    }
    if (w->lock) {
        vSemaphoreDelete(w->lock);
        w->lock = NULL;
    }
    heap_caps_free(w->rx);
    heap_caps_free(w->tx);
    w->rx = w->tx = NULL;
}

/* last event from the task: it's parked in vTaskSuspend() by now or about to be */
static void ev_closed(JSContext *ctx, void *arg)
{
    ws_t *w = arg;

    vTaskDeleteWithCaps(w->task);
    w->task = NULL;
    ws_release(w);
    w->state = ST_CLOSED;

    if (w->err[0]) {
        JSValue e = JS_NewString(ctx, w->err);
        qjs_esp_call(ctx, w->cb[CB_ERROR], w->h.obj, 1, &e);
        JS_FreeValue(ctx, e);
    }
    JSValue args[2] = { JS_NewInt32(ctx, w->close_code), JS_NewString(ctx, w->close_reason) };
    qjs_esp_call(ctx, w->cb[CB_CLOSE], w->h.obj, 2, args);
    JS_FreeValue(ctx, args[1]);
    qjs_esp_handle_unref(ctx, &w->h);   /* may free w */
}

/* ---- connection task ---- */

static void fail(ws_t *w, const char *msg)
{
    if (!w->err[0]) {
        snprintf(w->err, sizeof(w->err), "%s", msg);
    }
}

/* we bail out: tell the peer why, report 1006 like browsers do */
static void fail_with_close(ws_t *w, uint16_t code, const char *msg)
{
    uint8_t p[2] = { code >> 8, code & 0xff };
    ws_write(w, WS_TRANSPORT_OPCODES_CLOSE | WS_TRANSPORT_OPCODES_FIN, p, 2);
    fail(w, msg);
}

/* 1 = connection is done */
static int on_control(ws_t *w, uint8_t *p, int n)
{
    switch (w->frame_op) {
    case WS_TRANSPORT_OPCODES_PING:
        if (ws_write(w, WS_TRANSPORT_OPCODES_PONG | WS_TRANSPORT_OPCODES_FIN, p, n)) {
            fail(w, "pong failed");
            return 1;
        }
        return 0;
    case WS_TRANSPORT_OPCODES_CLOSE:
        w->close_code = n >= 2 ? (p[0] << 8 | p[1]) : 1005;
        if (n > 2) {
            memcpy(w->close_reason, p + 2, n - 2);  /* n <= 125, fits */
            w->close_reason[n - 2] = '\0';
        }
        if (w->state != ST_CLOSING) {
            /* peer started it: echo the code back, then we're done */
            ws_write(w, WS_TRANSPORT_OPCODES_CLOSE | WS_TRANSPORT_OPCODES_FIN, p, n >= 2 ? 2 : 0);
        }
        return 1;
    default:
        return 0;   /* pong */
    }
}

/* one transport read; 1 = connection is done */
static int read_some(ws_t *w)
{
    uint8_t ctl[128];   /* first chunk of every frame: control payloads are <= 125 */
    bool start = w->frame_left == 0;
    /* mid-frame: the size check at frame start guarantees the rest fits */
    uint8_t *dst = start ? ctl : w->rx + w->rx_len;
    int want = start ? 125 : w->frame_left;

    xSemaphoreTake(w->lock, portMAX_DELAY);
    int n = esp_transport_read(w->ws, (char *)dst, want, IO_TIMEOUT_MS);
    int op = esp_transport_ws_get_read_opcode(w->ws);
    int len = esp_transport_ws_get_read_payload_len(w->ws);
    bool fin = esp_transport_ws_get_fin_flag(w->ws);
    xSemaphoreGive(w->lock);

    if (n < 0) {
        fail(w, "connection lost");
        return 1;
    }
    if (start) {
        if (op == WS_TRANSPORT_OPCODES_NONE) {
            return 0;   /* woke up for nothing */
        }
        w->frame_op = op;
        w->frame_fin = fin;
        w->frame_left = len;
    }
    w->frame_left -= n;

    if (w->frame_op & 0x08) {
        return on_control(w, ctl, n);   /* read whole by the transport */
    }

    if (start) {
        bool cont = w->frame_op == WS_TRANSPORT_OPCODES_CONT;
        if (cont != w->in_msg) {
            fail_with_close(w, 1002, "bad fragmentation");
            return 1;
        }
        if (!cont) {
            w->msg_op = w->frame_op;
            w->in_msg = true;
        }
        if (w->rx_len + len > RX_CAP) {
            fail_with_close(w, 1009, "message too big");
            return 1;
        }
        memcpy(w->rx + w->rx_len, ctl, n);
    }
    w->rx_len += n;

    if (w->frame_left == 0 && w->frame_fin) {
        qjs_esp_loop_post(w->loop, ev_message, w, portMAX_DELAY);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);    /* until ev_message copied it */
        w->rx_len = 0;
        w->in_msg = false;
    }
    return 0;
}

static void ws_task(void *arg)
{
    ws_t *w = arg;

    w->close_code = 1006;
    if (esp_transport_connect(w->ws, w->host, w->port, CONFIG_QJS_ESP_WS_CONNECT_TIMEOUT_MS) < 0) {
        int st = esp_transport_ws_get_upgrade_request_status(w->ws);
        if (st > 0) {
            snprintf(w->err, sizeof(w->err), "upgrade failed: HTTP %d", st);
        } else {
            fail(w, "connect failed");
        }
        goto out;
    }
    qjs_esp_loop_post(w->loop, ev_open, w, portMAX_DELAY);

    while (!w->abort) {
        if (w->state == ST_CLOSING && (int32_t)(xTaskGetTickCount() - w->close_deadline) >= 0) {
            break;      /* peer never answered our close, 1006 */
        }
        int r = esp_transport_poll_read(w->ws, POLL_MS);
        if (r < 0) {
            fail(w, "connection lost");
            break;
        }
        if (r > 0 && read_some(w)) {
            break;
        }
    }
out:
    xSemaphoreTake(w->lock, portMAX_DELAY);    /* JS may be mid-send */
    esp_transport_close(w->ws);
    w->dead = true;
    xSemaphoreGive(w->lock);
    qjs_esp_loop_post(w->loop, ev_closed, w, portMAX_DELAY);
    vTaskSuspend(NULL);     /* ev_closed deletes us */
}

/* captures the negotiated subprotocol, runs in the task during connect */
static void on_header(void *arg, const char *line, int len)
{
    static const char key[] = "Sec-WebSocket-Protocol:";
    ws_t *w = arg;

    if (len < (int)sizeof(key) || strncasecmp(line, key, sizeof(key) - 1)) {
        return;
    }
    line += sizeof(key) - 1;
    len -= sizeof(key) - 1;
    while (len > 0 && (*line == ' ' || *line == '\t')) {
        line++;
        len--;
    }
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r' || line[len - 1] == '\n')) {
        len--;
    }
    snprintf(w->protocol, sizeof(w->protocol), "%.*s", len, line);
}

/* ---- JS class ---- */

static void ws_detach(qjs_esp_handle_t *h)
{
    ws_t *w = container_of(h, ws_t, h);
    for (int i = 0; i < CB_MAX; i++) {
        JS_FreeValue(w->ctx, w->cb[i]);
        w->cb[i] = JS_UNDEFINED;
    }
    w->abort = true;    /* the task notices within POLL_MS, or when connect returns */
}

static void ws_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSClassID cid;
    ws_t *w = JS_GetAnyOpaque(val, &cid);
    if (!w) {
        return;
    }
    /* task => ref'd => can't be here. Only the constructor's error path is. */
    ws_release(w);
    for (int i = 0; i < CB_MAX; i++) {
        JS_FreeValueRT(rt, w->cb[i]);
    }
    js_free_rt(rt, w);
}

static void ws_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID cid;
    ws_t *w = JS_GetAnyOpaque(val, &cid);
    if (w) {
        for (int i = 0; i < CB_MAX; i++) {
            JS_MarkValue(rt, w->cb[i], mark_func);
        }
    }
}

static const JSClassDef ws_class = {
    "WebSocket",
    .finalizer = ws_finalizer,
    .gc_mark = ws_mark,
};

/* "Name: value\r\n"... from two parallel arrays. Constructor only. */
static char *build_headers(JSContext *ctx, JSValueConst names, JSValueConst values)
{
    int64_t n;
    size_t len = 0;
    char *buf = NULL;

    if (JS_GetLength(ctx, names, &n)) {
        return NULL;
    }
    for (int64_t i = 0; i < n; i++) {
        JSValue kv = JS_GetPropertyInt64(ctx, names, i);
        JSValue vv = JS_GetPropertyInt64(ctx, values, i);
        size_t kl, vl;
        const char *k = JS_ToCStringLen(ctx, &kl, kv);
        const char *v = k ? JS_ToCStringLen(ctx, &vl, vv) : NULL;
        JS_FreeValue(ctx, kv);
        JS_FreeValue(ctx, vv);
        char *nb = v ? js_realloc(ctx, buf, len + kl + vl + 5) : NULL;
        if (nb) {
            len += sprintf(nb + len, "%s: %s\r\n", k, v);
            buf = nb;
        }
        JS_FreeCString(ctx, k);
        JS_FreeCString(ctx, v);
        if (!nb) {
            js_free(ctx, buf);
            return NULL;
        }
    }
    return buf ? buf : js_strdup(ctx, "");
}

static bool scheme_is(const char *s, int len, const char *want)
{
    return len == (int)strlen(want) && !strncasecmp(s, want, len);
}

/* ws[s]://host[:port][/path][?query] -> w->host, w->port, js_malloc'd path */
static char *parse_url(JSContext *ctx, ws_t *w, const char *url, bool *tls)
{
    struct http_parser_url u;
    http_parser_url_init(&u);
    if (http_parser_parse_url(url, strlen(url), 0, &u) ||
        !(u.field_set & (1 << UF_SCHEMA)) || !(u.field_set & (1 << UF_HOST))) {
        JS_ThrowSyntaxError(ctx, "invalid WebSocket URL");
        return NULL;
    }
    const char *scheme = url + u.field_data[UF_SCHEMA].off;
    int scheme_len = u.field_data[UF_SCHEMA].len;
    *tls = scheme_is(scheme, scheme_len, "wss") || scheme_is(scheme, scheme_len, "https");
    if (!*tls && !scheme_is(scheme, scheme_len, "ws") && !scheme_is(scheme, scheme_len, "http")) {
        JS_ThrowSyntaxError(ctx, "unsupported scheme: %.*s", scheme_len, scheme);
        return NULL;
    }
    if (u.field_data[UF_HOST].len >= sizeof(w->host)) {
        JS_ThrowSyntaxError(ctx, "host name too long");
        return NULL;
    }
    memcpy(w->host, url + u.field_data[UF_HOST].off, u.field_data[UF_HOST].len);
    w->host[u.field_data[UF_HOST].len] = '\0';
    w->port = u.port ? u.port : (*tls ? 443 : 80);

    const char *p = "/", *q = "";
    int plen = 1, qlen = 0;
    if (u.field_set & (1 << UF_PATH)) {
        p = url + u.field_data[UF_PATH].off;
        plen = u.field_data[UF_PATH].len;
    }
    if (u.field_set & (1 << UF_QUERY)) {
        q = url + u.field_data[UF_QUERY].off - 1;   /* include the '?' */
        qlen = u.field_data[UF_QUERY].len + 1;
    }
    char *path = js_malloc(ctx, plen + qlen + 1);
    if (path) {
        memcpy(path, p, plen);
        memcpy(path + plen, q, qlen);
        path[plen + qlen] = '\0';
    }
    return path;
}

/* new WebSocket(url, protocols | null[, headerNames, headerValues]) */
static JSValue ws_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    JSValue obj = JS_NewObjectClass(ctx, qjs_esp_class_id(ctx, QJS_ESP_CLASS_WS));
    if (JS_IsException(obj)) {
        return obj;
    }
    ws_t *w = js_mallocz(ctx, sizeof(*w));
    if (!w) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    qjs_esp_handle_init(&w->h, ws_detach);
    w->ctx = ctx;
    w->loop = qjs_esp_loop_get(ctx);
    for (int i = 0; i < CB_MAX; i++) {
        w->cb[i] = JS_UNDEFINED;
    }
    JS_SetOpaque(obj, w);   /* from here on the finalizer cleans up after us */

    const char *url = NULL, *protos = NULL;
    char *headers = NULL, *path = NULL;
    bool tls;

    if (!(url = JS_ToCString(ctx, argv[0]))) {
        goto fail;
    }
    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]) && !(protos = JS_ToCString(ctx, argv[1]))) {
        goto fail;
    }
    if (argc > 3 && JS_IsArray(argv[2]) && !(headers = build_headers(ctx, argv[2], argv[3]))) {
        goto fail;
    }
    if (!(path = parse_url(ctx, w, url, &tls))) {
        goto fail;
    }

    w->rx = qjs_esp_buf_alloc(RX_CAP);
    w->tx = qjs_esp_buf_alloc(TX_CAP);
    w->lock = xSemaphoreCreateMutex();
    w->tcp = tls ? esp_transport_ssl_init() : esp_transport_tcp_init();
    w->ws = w->tcp ? esp_transport_ws_init(w->tcp) : NULL;
    if (!w->rx || !w->tx || !w->lock || !w->ws) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    if (tls) {
        esp_transport_ssl_crt_bundle_attach(w->tcp, esp_crt_bundle_attach);
    }
    esp_transport_ws_config_t cfg = {
        .ws_path = path,
        .sub_protocol = protos,
        .headers = headers && headers[0] ? headers : NULL,
        .header_hook = on_header,
        .header_user_context = w,
        .propagate_control_frames = true,   /* we want close codes, and answer pings ourselves */
    };
    if (esp_transport_ws_set_config(w->ws, &cfg) != ESP_OK) {    /* copies the strings */
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }

    qjs_esp_handle_ref(ctx, &w->h, obj);
    if (xTaskCreateWithCaps(ws_task, "qjs_ws", CONFIG_QJS_ESP_WS_TASK_STACK, w,
                            CONFIG_QJS_ESP_WS_TASK_PRIO, &w->task, TASK_CAPS) != pdPASS) {
        w->task = NULL;
        qjs_esp_handle_unref(ctx, &w->h);
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    JS_FreeCString(ctx, url);
    JS_FreeCString(ctx, protos);
    js_free(ctx, headers);
    js_free(ctx, path);
    return obj;

fail:
    JS_FreeCString(ctx, url);
    JS_FreeCString(ctx, protos);
    js_free(ctx, headers);
    js_free(ctx, path);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue ws_cb_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    ws_t *w = ws_get(ctx, this_val);
    return w ? JS_DupValue(ctx, w->cb[magic]) : JS_EXCEPTION;
}

static JSValue ws_cb_set(JSContext *ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    ws_t *w = ws_get(ctx, this_val);
    if (!w) {
        return JS_EXCEPTION;
    }
    if (JS_IsFunction(ctx, v) || JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, w->cb[magic]);
        w->cb[magic] = JS_DupValue(ctx, v);
    }
    return JS_UNDEFINED;
}

static JSValue ws_send(JSContext *ctx, ws_t *w, int op, const uint8_t *p, size_t n)
{
    if (w->state != ST_OPEN) {
        return JS_ThrowTypeError(ctx, "WebSocket is not open");
    }
    size_t off = 0;
    do {
        size_t k = n - off < TX_CAP ? n - off : TX_CAP;
        int fop = (off == 0 ? op : WS_TRANSPORT_OPCODES_CONT) | (off + k == n ? WS_TRANSPORT_OPCODES_FIN : 0);
        memcpy(w->tx, p + off, k);
        if (ws_write(w, fop, w->tx, k)) {
            w->abort = true;    /* the stream is broken mid-message, let the task wrap up */
            return JS_ThrowInternalError(ctx, "WebSocket send failed");
        }
        off += k;
    } while (off < n);
    return JS_UNDEFINED;
}

static JSValue ws_send_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ws_t *w = ws_get(ctx, this_val);
    size_t n;
    const char *s = w ? JS_ToCStringLen(ctx, &n, argv[0]) : NULL;
    if (!s) {
        return JS_EXCEPTION;
    }
    JSValue r = ws_send(ctx, w, WS_TRANSPORT_OPCODES_TEXT, (const uint8_t *)s, n);
    JS_FreeCString(ctx, s);
    return r;
}

/* sendBinary(arraybuffer | view[, offset[, length]]) */
static JSValue ws_send_binary(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ws_t *w = ws_get(ctx, this_val);
    uint64_t off = 0, len;
    uint8_t *p;
    size_t n;

    if (!w) {
        return JS_EXCEPTION;
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToIndex(ctx, &off, argv[1])) {
        return JS_EXCEPTION;
    }
    bool has_len = argc > 2 && !JS_IsUndefined(argv[2]);
    if (has_len && JS_ToIndex(ctx, &len, argv[2])) {
        return JS_EXCEPTION;
    }
    if (qjs_esp_get_bytes(ctx, argv[0], &p, &n)) {     /* last: the conversions above can run JS */
        return JS_EXCEPTION;
    }
    if (off > n || (has_len && len > n - off)) {
        return JS_ThrowRangeError(ctx, "out of bounds");
    }
    return ws_send(ctx, w, WS_TRANSPORT_OPCODES_BINARY, p + off, has_len ? len : n - off);
}

/* close([code[, reason]]) */
static JSValue ws_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ws_t *w = ws_get(ctx, this_val);
    uint8_t p[125];
    size_t n = 0;

    if (!w) {
        return JS_EXCEPTION;
    }
    if (w->state == ST_CONNECTING) {
        w->state = ST_CLOSING;
        w->abort = true;    /* fails the connection: error + close(1006) */
        return JS_UNDEFINED;
    }
    if (w->state != ST_OPEN) {
        return JS_UNDEFINED;
    }
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        int32_t code;
        if (JS_ToInt32(ctx, &code, argv[0])) {
            return JS_EXCEPTION;
        }
        p[0] = code >> 8;
        p[1] = code & 0xff;
        n = 2;
        if (argc > 1 && !JS_IsUndefined(argv[1])) {
            size_t rl;
            const char *r = JS_ToCStringLen(ctx, &rl, argv[1]);
            if (!r) {
                return JS_EXCEPTION;
            }
            if (rl > sizeof(p) - 2) {
                JS_FreeCString(ctx, r);
                return JS_ThrowRangeError(ctx, "close reason longer than 123 bytes");
            }
            memcpy(p + 2, r, rl);
            n += rl;
            JS_FreeCString(ctx, r);
        }
    }
    w->close_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(IO_TIMEOUT_MS);
    w->state = ST_CLOSING;
    if (ws_write(w, WS_TRANSPORT_OPCODES_CLOSE | WS_TRANSPORT_OPCODES_FIN, p, n)) {
        w->abort = true;
    }
    return JS_UNDEFINED;
}

static JSValue ws_buffered_amount(JSContext *ctx, JSValueConst this_val)
{
    return ws_get(ctx, this_val) ? JS_NewInt32(ctx, 0) : JS_EXCEPTION;
}

static JSValue ws_extensions(JSContext *ctx, JSValueConst this_val)
{
    return ws_get(ctx, this_val) ? JS_NewString(ctx, "") : JS_EXCEPTION;
}

static const JSCFunctionListEntry ws_proto[] = {
    JS_CGETSET_DEF("bufferedAmount", ws_buffered_amount, NULL),
    JS_CGETSET_DEF("extensions", ws_extensions, NULL),
    JS_CGETSET_MAGIC_DEF("onclose", ws_cb_get, ws_cb_set, CB_CLOSE),
    JS_CGETSET_MAGIC_DEF("onerror", ws_cb_get, ws_cb_set, CB_ERROR),
    JS_CGETSET_MAGIC_DEF("onmessage", ws_cb_get, ws_cb_set, CB_MESSAGE),
    JS_CGETSET_MAGIC_DEF("onopen", ws_cb_get, ws_cb_set, CB_OPEN),
    JS_CFUNC_DEF("close", 2, ws_close),
    JS_CFUNC_DEF("sendBinary", 3, ws_send_binary),
    JS_CFUNC_DEF("sendText", 1, ws_send_text),
};

static int ws_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (qjs_esp_class_init(ctx, QJS_ESP_CLASS_WS, &ws_class)) {
        return -1;
    }
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, ws_proto, countof(ws_proto));
    JSValue ctor = JS_NewCFunction2(ctx, ws_ctor, "WebSocket", 4, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, qjs_esp_class_id(ctx, QJS_ESP_CLASS_WS), proto);
    return JS_SetModuleExport(ctx, m, "WebSocket", ctor);
}

JSModuleDef *qjs_esp_init_module_ws(JSContext *ctx, const char *name)
{
    JSModuleDef *m = JS_NewCModule(ctx, name, ws_module_init);
    if (m) {
        JS_AddModuleExport(ctx, m, "WebSocket");
    }
    return m;
}

#endif /* CONFIG_QJS_ESP_MOD_WS */
