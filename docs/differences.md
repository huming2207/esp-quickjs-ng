# Differences from upstream

## Missing

ESP-IDF has no processes, pipes, ttys or symlinks, so these don't exist:

- `os.Worker`, `os.exec`, `waitpid`, `kill`, `pipe`, `dup`
- `std.popen`, `std.urlGet`, `std.tmpfile`, the tty functions
- `os.symlink`, `readlink`, `lstat`, `realpath`, `os.mkdtemp`, `mkstemp`, `os.cputime`
- `.so` modules

`os.platform` is `"esp-idf"`.

## Different

- `os.setReadHandler()` / `setWriteHandler()` only work with upstream's `js_std_loop()`. With
  `qjs_esp_loop_run()`, use posix-socket's `poll()`.
- No global `setTimeout` unless you call `qjs_esp_add_web_globals()`; otherwise it's
  `os.setTimeout`.
- `os.now()` is microseconds, `performance.now()` milliseconds.
- `os.sleep()` blocks in `vTaskDelay()` and never busy-waits, rounded up to whole ticks.

## How the port works

`quickjs/` is the upstream repo, untouched. The changes are applied at configure time by
`port/quickjs_libc.cmake`, which writes patched copies of `quickjs-libc.c` and `quickjs.c` into
the build dir:

- ESP-IDF is treated like WASI, whose gates already drop most of what can't work.
- `os.platform` says `"esp-idf"`.
- No `os.Worker`.
- `lstat()` gets a prototype, for newlib.
- `JS_INTERRUPT_COUNTER_INIT` can be overridden (`CONFIG_QJS_ESP_INTERRUPT_INTERVAL`).

Each patch anchors on exact upstream text and stops the build if it's gone. Bumping QuickJS is
`git -C quickjs checkout vX.Y.Z` and a rebuild; if the build stops, fix the anchor.

`port/libc_glue.c` fills in `nanosleep`, `utimes` and `lstat` as weak symbols.

## The aliasing trap

On the ESP toolchains `int32_t` is `long`, not `int`. Upstream casts `int *` to `int32_t *` in
plenty of places, which breaks strict aliasing, and at `-O2` GCC really does miscompile it:
`[...'abc']` loops forever until it runs out of memory.

So QuickJS is built with `-fno-strict-aliasing`. Don't drop it. Espressif's own quickjs-ng
component probably has the same latent bug.
