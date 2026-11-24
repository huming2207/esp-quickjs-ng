# udp

```js
import { UDP } from 'esp:udp';

const u = new UDP();                          // new UDP(AF_INET6) for v6
u.bind({ ip: '0.0.0.0', port: 9000 });        // flags: UDP_REUSEADDR | UDP_IPV6ONLY
u.onrecv = ({ data, addr, partial }, err) => {
    // data is a Uint8Array, addr is { ip, port }
};
u.startRecv();                                // binds to any:0 if you didn't
u.send(bytes, { ip: '10.0.0.2', port: 9000 });   // returns bytes sent
u.stopRecv();
u.close();
```

Also there: `connect`, `getsockname`, `getpeername`, `fileno`, `setBroadcast`, `setMembership`,
`setMulticastLoop`, `setMulticastTTL`, `setMulticastInterface`.

## Versus txiki.js

Same surface as txiki's internal UDP object, with these differences:

- `send()` is synchronous. lwIP UDP never blocks, it fails with `ENOMEM` instead, so there's no
  `onsend`.
- A datagram bigger than `CONFIG_QJS_ESP_RX_BUF_SIZE` (2 KB) arrives cut off, with
  `partial: true`.

## Notes

- Reads happen in the JS task, through one receive buffer shared by all sockets.
- At most `CONFIG_QJS_ESP_IO_MAX_WATCHERS` sockets receiving at once.
- IPv6 scoped addresses work (`fe80::1%2`) if lwIP has IPv6.
