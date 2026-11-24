# esp-quickjs-ng docs

QuickJS-NG on ESP-IDF, with an event loop that actually sleeps, a few network modules, and a box
to run code you don't trust.

- [Getting started](getting-started.md): add the component, run a script.
- [Event loop](event-loop.md): how JS, timers, sockets and your C code share one task.
- [Untrusted code](untrusted-code.md): the arena, time limits, stopping a script.
- [Bytecode](bytecode.md): compile on a PC or on the chip, load it later.
- [udp](udp.md), [WebSocket](websocket.md), [posix-socket](posix-socket.md): the native modules.
- [fetch and web globals](fetch.md): `fetch()`, timers, `TextEncoder`, `AbortController`.
- [Kconfig](kconfig.md): every option, one line each.
- [Differences from upstream](differences.md): what's missing, and the aliasing trap.
- [Testing](testing.md): the examples and esp-emu.
