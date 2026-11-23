# esp-quickjs-ng

[QuickJS-NG](https://github.com/quickjs-ng/quickjs) (v0.18.0) as an ESP-IDF component, with:

- a FreeRTOS event loop instead of quickjs-libc's `poll()` loop
- `udp`, `ws` (WebSocket client) and `posix-socket` native modules, ported from
  [txiki.js](https://github.com/saghul/txiki.js) off libuv / libwebsockets onto lwIP and `esp_transport`
- the libc glue quickjs-libc needs on ESP-IDF, and a PSRAM-aware runtime helper

Needs ESP-IDF >= 6.0. Tested on ESP32-S3 (8 MB octal PSRAM) and ESP32-S31 (16 MB PSRAM) under
[esp-emu](https://github.com/espressif/esp-emulator); also builds for ESP32-P4.

## Getting it

```sh
git clone --recursive https://github.com/huming2207/esp-quickjs-ng components/esp-quickjs-ng
```

`quickjs/` is an untouched submodule. The few ESP-IDF gates quickjs-libc.c needs are applied at
configure time by `port/quickjs_libc.cmake` into the build dir; it stops the build if upstream moved
things, so bumping QuickJS is `git -C quickjs checkout vX.Y.Z` and a rebuild.

## Using it

```c
#include "quickjs-libc.h"
#include "qjs_esp.h"

static void js_task(void *arg)
{
    JSRuntime *rt = qjs_esp_new_runtime();      /* heap_caps allocator, stack limit from this task */
    js_std_init_handlers(rt);
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);
    js_init_module_std(ctx, "qjs:std");
    js_init_module_os(ctx, "qjs:os");
    qjs_esp_init_module_udp(ctx, "esp:udp");
    qjs_esp_init_module_ws(ctx, "esp:ws");
    qjs_esp_init_module_posix_socket(ctx, "esp:posix-socket");
    qjs_esp_loop_t *loop = qjs_esp_loop_new(ctx);

    JSValue ret = JS_Eval(ctx, src, len, "main.js", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(ret)) {
        js_std_dump_error(ctx);
    } else {
        JS_FreeValue(ctx, ret);
        qjs_esp_loop_run(loop);                 /* until no timers, jobs or open handles */
    }

    qjs_esp_loop_free(loop);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    vTaskDelete(NULL);
}

/* JS recursion eats C stack, give it room */
xTaskCreate(js_task, "js", 16 * 1024, NULL, 5, NULL);
```

`qjs_esp_loop_post(loop, cb, arg, wait)` runs `cb(ctx, arg)` in the JS task, from any other task.

See `examples/basic`, `examples/network` and `examples/bench`.

## How the loop works

```
            ┌──────────── FreeRTOS queue ────────────┐
 I/O task ──┤ socket ready (select on watched fds)   │
 ws task  ──┤ open / message / closed                ├──> JS task: js_std_loop_once() for
 app code ──┤ qjs_esp_loop_post()                    │    timers + jobs, then xQueueReceive()
            └────────────────────────────────────────┘    until the next timer is due
```

- **JS task** never polls. It runs quickjs-libc's timers and promise jobs via
  `js_std_loop_once()`, then sleeps on the queue until the next timer.
- **I/O task** (one, shared): `select()` over the UDP / posix sockets that are being watched, woken by
  an ESP-IDF eventfd when the set changes. It only reports readiness; reads and writes happen in the
  JS task. A watcher is disarmed when it fires and re-armed after its callback, so a busy socket
  doesn't spin it.
- **ws task** (one per open WebSocket): `esp_transport` is blocking (DNS, TLS handshake, frame reads),
  so it lives in its own task, not the JS one. A mutex around the transport keeps mbedTLS from being
  read and written at once. Sends are synchronous from the JS task; while the task is mid-read of a
  slow frame a send can wait up to 2x `CONFIG_QJS_ESP_WS_IO_TIMEOUT_MS`. Idle, the task blocks in
  `select()` on its socket plus an eventfd the JS task pokes on `close()`.
- Idle means blocked: no task polls, so with tickless idle and automatic light sleep the chip sleeps
  until a timer, a packet or an app event. `examples/network` measures it (0 µs of CPU for the
  I/O and ws tasks over 3 s idle). `os.sleep()` blocks in `vTaskDelay()` too, rounded up to whole
  ticks, never busy-waits.
- Unhandled promise rejections are printed, not `exit(1)`, once the job queue is empty.
  Uncaught errors in callbacks are printed and the loop keeps going.

Heap use after init is fixed: the event queue, a shared receive buffer, and per-WebSocket rx/tx
buffers are allocated up front. Nothing in the C code mallocs per packet or per message; the JS
values handed to callbacks are of course allocated by QuickJS.

## Modules

The JS surface follows txiki.js's internal native objects, so its JS wrappers can sit on top.

### `udp`

```js
import { UDP } from 'esp:udp';
const u = new UDP();                       // or new UDP(AF_INET6)
u.bind({ ip: '0.0.0.0', port: 9000 });     // flags: UDP_REUSEADDR | UDP_IPV6ONLY
u.onrecv = ({ data, addr, partial }, err) => { /* data: Uint8Array */ };
u.startRecv();                             // binds to any:0 if not bound yet
u.send(bytes, { ip: '10.0.0.2', port: 9000 });   // -> bytes sent, synchronous
u.stopRecv(); u.close();
```

Also `connect`, `getsockname`, `getpeername`, `fileno`, `setBroadcast`, `setMembership`,
`setMulticastLoop`, `setMulticastTTL`, `setMulticastInterface`.
Differences from txiki: `send()` is synchronous (lwIP UDP never blocks, it fails with `ENOMEM`), so
there's no `onsend`. Datagrams bigger than `CONFIG_QJS_ESP_RX_BUF_SIZE` arrive truncated with
`partial: true`.

### `ws`

```js
import { WebSocket } from 'esp:ws';
const ws = new WebSocket('wss://example.com/chat', 'proto', ['Authorization'], ['Bearer x']);
ws.onopen = protocol => ws.sendText('hi');
ws.onmessage = data => {};                 // string or ArrayBuffer
ws.onerror = reason => {};
ws.onclose = (code, reason) => {};
ws.sendBinary(buf, offset, length);
ws.close(1000, 'bye');
```

`wss://` verifies against the ESP-IDF certificate bundle. Messages bigger than
`CONFIG_QJS_ESP_WS_MAX_MESSAGE` fail the connection with 1009. `bufferedAmount` is always 0 (sends
are synchronous), `extensions` always `""` (esp_transport has no permessage-deflate).
Sending while still connecting throws, like browsers.

### `posix-socket`

txiki's `posixSocketLoad()` namespace as a module: `PosixSocket`, `create_sockaddr_inet`,
`socket_from_fd`, `defines`, `uv_poll_event_bits`, `uv_strerror`/`strerror`, `if_nametoindex`,
`if_indextoname`, `checksum`. Socket methods: `bind`, `connect`, `listen`, `accept`, `read`, `write`,
`recv`, `recvmsg`, `sendmsg`, `setopt`, `getopt`, `shutdown`, `poll(events, cb)`, `pollStop`, `close`,
plus `setNonBlocking(on)` (new: without it `connect()`/`accept()` block the JS task).
`DISCONNECT` is detected by peeking for EOF; lwIP has no OOB data, so `PRIORITIZED` never fires.
`defines` also carries `EAGAIN`, `EINPROGRESS` and friends, since errno values differ per libc.

## What's different from upstream qjs

- `os.Worker`, `os.exec`/`waitpid`/`kill`/`pipe`/`dup`, `std.popen`/`urlGet`/`tmpfile`, tty functions,
  `os.symlink`/`readlink`/`lstat`/`realpath`, `os.mkdtemp`/`mkstemp`, `os.cputime` and `.so` modules
  don't exist. `os.platform` is `"esp-idf"`.
- `os.setReadHandler()` / `setWriteHandler()` only work with upstream's `js_std_loop()`, not with
  `qjs_esp_loop_run()`. Use `posix-socket`'s `poll()`.
- As upstream: no global `setTimeout`, use `os.setTimeout`. `os.now()` is in microseconds;
  `performance.now()` is milliseconds.
- `nanosleep` (for `os.sleep`, `vTaskDelay` only), `utimes` and `lstat` are provided as weak
  symbols in `port/libc_glue.c`.

### `-fno-strict-aliasing`, and why it matters here

On the ESP toolchains `int32_t` is `long` and `uint32_t` is `unsigned long`. Upstream casts between
`int *` and `int32_t *` / `uint32_t *` in many places (`JS_ToInt32(ctx, &some_int, v)`,
`string_getc(p, (int *)&idx)`), which breaks strict aliasing. At `-O2` this is a real miscompile:
iterating a string (`[...'abc']`) never advances and eats the heap until "out of memory".
QuickJS sources are built with `-fno-strict-aliasing`; don't drop it.

## Kconfig

`Component config → QuickJS-NG`:

| Option | Default | |
|---|---|---|
| `QJS_ESP_HEAP_PREFER_PSRAM` | y | JS heap in PSRAM, falls back to internal |
| `QJS_ESP_STACK_MARGIN` | 4096 | stack kept for native code below the JS recursion limit |
| `QJS_ESP_EVENT_QUEUE_LEN` | 16 | |
| `QJS_ESP_MOD_UDP` / `_POSIX_SOCKET` / `_WS` | y | |
| `QJS_ESP_IO_MAX_WATCHERS` | 8 | sockets watched at once |
| `QJS_ESP_IO_TASK_STACK` / `_PRIO` | 3072 / 5 | |
| `QJS_ESP_RX_BUF_SIZE` | 2048 | shared receive buffer |
| `QJS_ESP_WS_MAX_MESSAGE` | 4096 | per-connection rx buffer |
| `QJS_ESP_WS_TX_CHUNK` | 1024 | per-connection tx buffer, bigger messages are fragmented |
| `QJS_ESP_WS_TASK_STACK` / `_PRIO` | 6144 / 5 | per-connection task (DNS + TLS handshake run here) |
| `QJS_ESP_WS_TASK_STACK_PSRAM` | n | put those stacks in PSRAM |
| `QJS_ESP_WS_CONNECT_TIMEOUT_MS` | 10000 | |
| `QJS_ESP_WS_IO_TIMEOUT_MS` | 5000 | send / partial frame / close handshake timeout |

## Running the examples in esp-emu

```sh
cd examples/network
idf.py --preview set-target esp32s31 build
cd build && esptool.py --chip esp32s31 merge-bin --format raw -o merged.bin @flash_args
python3 ../tools/echo_server.py &          # UDP 9000, TCP 9001, WS 9002
esp-emu --chip esp32s31 --firmware merged.bin --psram-size 16M \
        --net user,net=10.0.2.0/24 --exit-on "network: done"
```

The example's `sdkconfig.defaults` points Wi-Fi at esp-emu's soft AP (`myssid` / `mypassword`) and
the echo host at `10.0.2.1`; change both for real hardware.
