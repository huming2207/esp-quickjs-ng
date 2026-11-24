# posix-socket

Raw BSD sockets, for when `udp` and `ws` aren't enough (TCP, servers, odd options). It's txiki's
`posixSocketLoad()` namespace as a module.

```js
import { PosixSocket, create_sockaddr_inet, defines } from 'esp:posix-socket';
```

Exports: `PosixSocket`, `create_sockaddr_inet`, `socket_from_fd`, `defines`,
`uv_poll_event_bits`, `uv_strerror` / `strerror`, `if_nametoindex`, `if_indextoname`, `checksum`.

Socket methods: `bind`, `connect`, `listen`, `accept`, `read`, `write`, `recv`, `recvmsg`,
`sendmsg`, `setopt`, `getopt`, `shutdown`, `poll(events, cb)`, `pollStop`, `close`.

## Things to know

- **`setNonBlocking(true)` first.** It's new here, and without it `connect()` and `accept()` block
  the JS task, and with it the whole loop.
- `poll(events, cb)` is how you wait for readiness. `os.setReadHandler()` doesn't work with this
  loop.
- `DISCONNECT` is found by peeking for EOF. lwIP has no out-of-band data, so `PRIORITIZED` never
  fires.
- `defines` also has `EAGAIN`, `EINPROGRESS` and friends. errno numbers differ per libc, so don't
  hardcode them.
