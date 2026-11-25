# Untrusted code

For scripts that might be garbage, say from a weak LLM or a user. The goal: the script dies, the
device doesn't.

## The recipe

```c
// once, at boot; never freed
qjs_esp_arena_t *ar = qjs_esp_arena_new(2 * 1024 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

// per script, in the JS task
JSRuntime *rt = qjs_esp_new_runtime_in(ar);
...
qjs_esp_loop_set_limits(loop, &(qjs_esp_limits_t){ .slice_ms = 200, .yield_ms = 20 });
qjs_esp_loop_set_deadline(loop, esp_timer_get_time() + 30 * 1000000LL);
// eval
qjs_esp_stop_t why = qjs_esp_loop_run(loop);
qjs_esp_loop_free(loop);
if (why == QJS_ESP_STOP_IDLE) {
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
} else {
    qjs_esp_arena_reset(ar);   // don't walk a runtime you just killed
}
```

## Memory: the arena

Everything the runtime allocates comes from the arena's own TLSF heap, nothing else. There's no
fallback to internal RAM. QuickJS throws "out of memory" a little before the arena is full, so it
still has room to unwind.

The script can't catch it: QuickJS-NG makes every `InternalError` uncatchable, "out of memory"
among them, and an async function that dies of one never settles its promise. So the loop stops
there: `qjs_esp_loop_run()` returns `QJS_ESP_STOP_ERROR` and `qjs_esp_loop_error()` has the text.
A host that gets an uncatchable error back from its own `JS_Call()` hands it to
`qjs_esp_loop_fail()`, which does the same unless the error is the loop's own interrupt
(`qjs_esp_loop_stopping()` is non-zero then).

- Grab it once at boot, it's never freed. Up to 4 arenas.
- One runtime per arena at a time: `qjs_esp_new_runtime_in()` refuses a non-empty arena.
- `qjs_esp_arena_used()` / `_peak()` for numbers. A bare runtime is about 160 KB.

## Time

- **`slice_ms`**: the longest one turn may run. A turn is one callback, or a batch of promise jobs
  plus one timer. It catches `while (true) {}`, endless promise chains, regex blowups.
- **deadline**: an absolute `esp_timer_get_time()`, for the whole run, idle or not.
- **`yield_ms`**: a busy script does `vTaskDelay(1)` this often, so lower-priority tasks, the idle
  task and the task watchdog still get a go.
- **`qjs_esp_loop_stop()`**: from any task, ends it now.

Past any of them, the running JS gets an uncatchable interrupt (`try/catch` doesn't save it) and
`qjs_esp_loop_run()` returns why.

## How the checks run

QuickJS counts interrupt points (loop back-jumps and calls) and calls our handler every
`CONFIG_QJS_ESP_INTERRUPT_INTERVAL` of them (100 here, 10000 upstream). The handler checks a stop
flag, then reads the CPU cycle counter, and only reads the real clock about once a millisecond.
The cycle counter is just a cheap gate: it's sized for XTAL (the slowest clock DFS uses) and reset
whenever the JS task wakes, so DFS, light sleep and core switches only make it look at the clock
early, never late.

The arena allocator checks too: the stop flag on every allocation, the clock on every 16th. Once
stopped, allocations fail.

## The gap

Builtins that neither allocate nor count as interrupt points slip through, for example `fill()` on
an existing array or `indexOf()` on a big string. A loop of them overruns the slice by about
`INTERRUPT_INTERVAL / 2` calls. In esp-emu, with a 200 ms slice:

| Interval | `for (;;) a.fill(2)` over 200k elements stops after |
|---|---|
| 1000 | 19.7 s |
| 100 (default) | ~1.9 s on S31, ~4.6 s on S3 |
| 10 | 366 ms |

Real chips are slower than esp-emu, so expect worse. The cost of 100 in `examples/bench` is under
0.3%; 10 costs about 1%, 1 about 12%.

## Teardown

`JS_FreeRuntime()` asserts on any leaked object. After a stop, the runtime may be mid-call and not
free cleanly, so reset the arena instead. Always `qjs_esp_loop_free()` first: the I/O and ws tasks
point into the arena. Only arena memory comes back; a `FILE` opened through `qjs:std` leaks.

## Also do

- Don't hand untrusted code `qjs:std` / `qjs:os` unless you want it touching files.
- Build its context with `JS_NewContextRaw()` and only the intrinsics it needs (no `Eval` if it's
  bytecode only).
- `examples/guard` throws about 20 hostile scripts at all of this and checks the system heap is
  untouched after each.
