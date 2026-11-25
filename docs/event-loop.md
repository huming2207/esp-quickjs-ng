# Event loop

quickjs-libc's own loop is a `poll()` loop, which ESP-IDF can't really do. This one blocks on a
FreeRTOS queue instead.

```
 I/O task  ── socket ready ──┐
 ws tasks  ── open/msg/close ┼──> queue ──> JS task: timers + promise jobs,
 your code ── loop_post() ───┘                      then block until the next timer
```

- **JS task**: runs timers and promise jobs (`js_std_loop_once()`), then sleeps on the queue until
  something arrives or the next timer is due. It never polls.
- **I/O task**: one, shared. `select()`s on the sockets JS is watching and only says "ready"; reads
  and writes happen in the JS task.
- **ws tasks**: one per open WebSocket, because `esp_transport` blocks (DNS, TLS, reads).

## Idle means blocked

Nothing spins. With tickless idle and auto light sleep the chip sleeps until a timer, a packet, or
your code posts something. `examples/network` measures it: 0 µs of CPU in the I/O and ws tasks
over 3 s idle. `os.sleep()` is `vTaskDelay()`, rounded up to whole ticks.

## Talking to JS from C

```c
qjs_esp_loop_post(loop, cb, arg, wait);   // any task, not ISRs
```

`cb(ctx, arg)` runs in the JS task. That's the only safe way to touch JS from another task.

## When it returns

`qjs_esp_loop_run()` returns when there are no timers, no jobs and no open handles left, or when
something stopped it. It tells you which:

| Return | Why |
|---|---|
| `QJS_ESP_STOP_IDLE` | nothing left to do |
| `QJS_ESP_STOP_REQUESTED` | someone called `qjs_esp_loop_stop()` |
| `QJS_ESP_STOP_DEADLINE` | past `qjs_esp_loop_set_deadline()` |
| `QJS_ESP_STOP_SLICE` | one turn ran longer than `slice_ms` |
| `QJS_ESP_STOP_ERROR` | an uncatchable error the loop didn't cause, e.g. out of memory; `qjs_esp_loop_error()` has its text |

Anything but `IDLE` is final: free the loop. See [Untrusted code](untrusted-code.md) for the
limits.

## Keep-alive

A host that calls into a script now and then (hooks, commands) doesn't want the loop to return
when the script goes quiet:

```c
qjs_esp_loop_keep_alive(loop, true);   // block for the next post instead of returning
...
qjs_esp_loop_stop(loop);               // end it
```

## Errors

- An uncaught error in a callback is printed and the loop keeps going.
- Unhandled promise rejections are printed once the job queue is empty, not `exit(1)` like
  upstream.
- To see them yourself, e.g. to send them to whoever wrote the script, set a handler. It runs in
  the JS task before the print, and not for the interrupt that stops the loop:

```c
static void on_error(JSContext *ctx, qjs_esp_uncaught_t what, JSValueConst err, void *arg)
{
    // what: QJS_ESP_UNCAUGHT_ERROR or QJS_ESP_UNHANDLED_REJECTION; err is borrowed
}

qjs_esp_loop_set_error_handler(loop, on_error, NULL);
```

## Memory

After init the C side doesn't allocate per packet or per message. The event queue, one shared
receive buffer and the per-WebSocket buffers are all set up front. The JS values handed to
callbacks are QuickJS's business.

## Gotchas

- One loop per runtime. The loop uses the runtime opaque slot (`JS_SetRuntimeOpaque()`), so don't.
- `os.setReadHandler()` / `setWriteHandler()` don't work here. Use posix-socket's `poll()`.
- Freeing the loop with a WebSocket still connecting can take up to the connect timeout.
