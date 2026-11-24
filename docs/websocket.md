# WebSocket

A client, on `esp_transport`.

```js
import { WebSocket } from 'esp:ws';

const ws = new WebSocket('wss://example.com/chat', 'proto', ['Authorization'], ['Bearer x']);
ws.onopen = (protocol) => ws.sendText('hi');
ws.onmessage = (data) => {};          // string or ArrayBuffer
ws.onerror = (reason) => {};
ws.onclose = (code, reason) => {};
ws.sendBinary(buf, offset, length);
ws.close(1000, 'bye');
```

The extra headers are two arrays: names and values.

## Behaviour

- `wss://` checks the server against the ESP-IDF certificate bundle.
- Sending while it's still connecting throws, same as in browsers.
- `bufferedAmount` is always 0, because sends are synchronous. `extensions` is always `""` (no
  permessage-deflate in `esp_transport`).
- A message bigger than `CONFIG_QJS_ESP_WS_MAX_MESSAGE` (4 KB) closes the connection with 1009.
- Big sends go out as fragments of `CONFIG_QJS_ESP_WS_TX_CHUNK`.

## Under the hood

Each open socket gets its own task. DNS, the TLS handshake and reads all block, so they can't
live in the JS task. That task sleeps in `select()` on the socket plus an eventfd, so an idle
connection costs no CPU and doesn't stop light sleep. There's no periodic poll.

A send can wait for the connection lock while the task is in the middle of reading a slow frame:
up to 2 × `CONFIG_QJS_ESP_WS_IO_TIMEOUT_MS`. Fixing that would mean async, buffered sends.

## Limits

- At most `CONFIG_QJS_ESP_WS_MAX_OPEN` (4) open at once; one more fails with `EMFILE`.
- Each one costs a task stack (`CONFIG_QJS_ESP_WS_TASK_STACK`, 6 KB, can go in PSRAM) plus its rx
  and tx buffers.
