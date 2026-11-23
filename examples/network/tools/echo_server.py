#!/usr/bin/env python3
"""Echo servers for examples/network: UDP 9000, TCP 9001, WebSocket 9002.

Stdlib only. The WebSocket side is just enough RFC 6455 for the tests:
echoes frames as they come (so fragmentation survives the round trip), plus
"close-me" -> server closes with 4000, "huge" -> sends 8 KiB.
"""
import asyncio
import base64
import hashlib
import re
import struct

GUID = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'


class UdpEcho(asyncio.DatagramProtocol):
    def connection_made(self, transport):
        self.t = transport

    def datagram_received(self, data, addr):
        self.t.sendto(data, addr)


async def tcp_echo(r, w):
    while data := await r.read(4096):
        w.write(data)
        await w.drain()
    w.close()


def frame(op, payload, fin=True):
    n = len(payload)
    head = bytes([(0x80 if fin else 0) | op])
    if n < 126:
        head += bytes([n])
    elif n < 65536:
        head += bytes([126]) + struct.pack('!H', n)
    else:
        head += bytes([127]) + struct.pack('!Q', n)
    return head + payload


async def ws_echo(r, w):
    req = await r.readuntil(b'\r\n\r\n')
    key = re.search(rb'Sec-WebSocket-Key:\s*(\S+)', req, re.I).group(1)
    accept = base64.b64encode(hashlib.sha1(key + GUID).digest())
    resp = b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
    resp += b'Sec-WebSocket-Accept: ' + accept + b'\r\n'
    proto = re.search(rb'Sec-WebSocket-Protocol:\s*([^,\r]+)', req, re.I)
    if proto:
        resp += b'Sec-WebSocket-Protocol: ' + proto.group(1).strip() + b'\r\n'
    w.write(resp + b'\r\n')

    try:
        while True:
            h = await r.readexactly(2)
            fin, op, n = h[0] & 0x80, h[0] & 0x0f, h[1] & 0x7f
            if n == 126:
                n = struct.unpack('!H', await r.readexactly(2))[0]
            elif n == 127:
                n = struct.unpack('!Q', await r.readexactly(8))[0]
            mask = await r.readexactly(4) if h[1] & 0x80 else b'\0\0\0\0'
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(await r.readexactly(n)))

            if op == 0x8:                       # close: echo the code, hang up
                w.write(frame(0x8, data[:2]))
                break
            if op == 0x9:                       # ping
                w.write(frame(0xa, data))
            elif op == 0x1 and data == b'close-me':
                w.write(frame(0x8, struct.pack('!H', 4000) + b'server bye'))
                await r.read()                  # wait for the client's echo + FIN
                break
            elif op == 0x1 and data == b'huge':
                w.write(frame(0x2, bytes(8192)))
            elif op in (0x0, 0x1, 0x2):
                w.write(frame(op, data, bool(fin)))
            await w.drain()
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    w.close()


async def main():
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(UdpEcho, local_addr=('0.0.0.0', 9000))
    tcp = await asyncio.start_server(tcp_echo, '0.0.0.0', 9001)
    ws = await asyncio.start_server(ws_echo, '0.0.0.0', 9002)
    print('udp :9000, tcp :9001, ws :9002')
    async with tcp, ws:
        await asyncio.gather(tcp.serve_forever(), ws.serve_forever())


if __name__ == '__main__':
    asyncio.run(main())
