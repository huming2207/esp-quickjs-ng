# Kconfig

`Component config → QuickJS-NG`.

## Core

| Option | Default | What |
|---|---|---|
| `QJS_ESP_HEAP_PREFER_PSRAM` | y | `qjs_esp_new_runtime()` heap in PSRAM, falls back to internal |
| `QJS_ESP_STACK_MARGIN` | 4096 | stack kept for native code under the JS recursion limit |
| `QJS_ESP_INTERRUPT_INTERVAL` | 100 | interrupt points between interrupt handler runs (upstream 10000), see [Untrusted code](untrusted-code.md) |
| `QJS_ESP_EVENT_QUEUE_LEN` | 16 | JS task's event queue |

## Bytecode

| Option | Default | What |
|---|---|---|
| `QJS_ESP_BC_COMPILE` | y | `qjs_esp_bc_compile()` on the chip |
| `QJS_ESP_BC_LOAD` | y | `qjs_esp_bc_eval()` |

## Modules

| Option | Default | What |
|---|---|---|
| `QJS_ESP_MOD_UDP` | y | `esp:udp` |
| `QJS_ESP_MOD_POSIX_SOCKET` | y | `esp:posix-socket` |
| `QJS_ESP_MOD_WS` | y | `esp:ws` |
| `QJS_ESP_FETCH` | y | `fetch()`, `Headers`, `Response` |

## Sockets (udp, posix-socket)

| Option | Default | What |
|---|---|---|
| `QJS_ESP_IO_MAX_WATCHERS` | 8 | sockets watched at once |
| `QJS_ESP_IO_TASK_STACK` | 3072 | the I/O task's stack |
| `QJS_ESP_IO_TASK_PRIO` | 5 | |
| `QJS_ESP_RX_BUF_SIZE` | 2048 | shared receive buffer; bigger datagrams get cut |

## WebSocket

| Option | Default | What |
|---|---|---|
| `QJS_ESP_WS_MAX_OPEN` | 4 | open at once |
| `QJS_ESP_WS_MAX_MESSAGE` | 4096 | rx buffer per connection; bigger messages close it with 1009 |
| `QJS_ESP_WS_TX_CHUNK` | 1024 | tx buffer per connection; bigger messages get fragmented |
| `QJS_ESP_WS_TASK_STACK` | 6144 | per-connection task (DNS, TLS, reads) |
| `QJS_ESP_WS_TASK_STACK_PSRAM` | n | put those stacks in PSRAM |
| `QJS_ESP_WS_TASK_PRIO` | 5 | |
| `QJS_ESP_WS_CONNECT_TIMEOUT_MS` | 10000 | |
| `QJS_ESP_WS_IO_TIMEOUT_MS` | 5000 | sends, partial frames, the close handshake |

## fetch

| Option | Default | What |
|---|---|---|
| `QJS_ESP_FETCH_MAX_INFLIGHT` | 4 | requests running at once; the rest wait |
| `QJS_ESP_FETCH_MAX_BODY_KB` | 512 | biggest response body |
| `QJS_ESP_FETCH_TIMEOUT_MS` | 30000 | default per-request timeout |
| `QJS_ESP_FETCH_TASK_STACK` | 8192 | per-request task |
| `QJS_ESP_FETCH_TASK_PRIO` | 5 | |
