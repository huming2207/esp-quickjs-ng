// Talks to tools/echo_server.py: UDP echo on 9000, TCP echo on 9001,
// WebSocket echo on 9002.
import { UDP } from 'esp:udp';
import { WebSocket } from 'esp:ws';
import * as ps from 'esp:posix-socket';
import * as os from 'qjs:os';

const HOST = globalThis.ECHO_HOST;
const enc = s => Uint8Array.from(s, c => c.charCodeAt(0));   // ASCII only, fine here
const dec = b => String.fromCharCode(...b);

function check(what, ok) {
    console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`);
}

// quickjs-ng has no global setTimeout, it lives in qjs:os
function withTimeout(p, ms, what) {
    let id;
    const timeout = new Promise((_, rej) => { id = os.setTimeout(() => rej(new Error(`${what}: timeout`)), ms); });
    return Promise.race([p, timeout]).finally(() => os.clearTimeout(id));
}

async function udp() {
    const u = new UDP();
    const got = new Promise((resolve, reject) => {
        u.onrecv = (msg, err) => (err ? reject(err) : resolve(msg));
    });
    u.startRecv();
    const n = u.send(enc('hello udp'), { ip: HOST, port: 9000 });
    const msg = await withTimeout(got, 3000, 'udp');
    u.close();
    check(`udp: sent ${n} bytes, echo "${dec(msg.data)}" from ${msg.addr.ip}:${msg.addr.port}`,
          dec(msg.data) === 'hello udp' && msg.addr.port === 9000 && !msg.partial);
}

// non-blocking connect + poll(), the way you'd do it without blocking the loop
async function tcp() {
    const { defines: D, uv_poll_event_bits: EV } = ps;
    const s = new ps.PosixSocket(D.AF_INET, D.SOCK_STREAM, 0);
    s.setNonBlocking(true);
    try {
        s.connect(ps.create_sockaddr_inet({ ip: HOST, port: 9001 }));
    } catch (e) {
        if (e.errno !== D.EINPROGRESS) {
            throw e;
        }
    }
    await withTimeout(new Promise(resolve => s.poll(EV.WRITABLE, () => { s.pollStop(); resolve(); })), 3000, 'tcp connect');
    const soErr = new DataView(s.getopt(D.SOL_SOCKET, D.SO_ERROR, 4).buffer).getInt32(0, true);
    check('tcp: connected', soErr === 0);

    s.write(enc('hello tcp'));
    const data = await withTimeout(new Promise(resolve => {
        s.poll(EV.READABLE | EV.DISCONNECT, (status, ev) => {
            s.pollStop();
            resolve(s.read(64));
        });
    }), 3000, 'tcp read');
    s.close();
    check(`tcp: echo "${dec(data)}"`, dec(data) === 'hello tcp');
}

function wsSession(url, steps) {
    return withTimeout(new Promise((resolve, reject) => {
        const ws = new WebSocket(url, 'echo');
        const log = [];
        ws.onopen = proto => {
            log.push(`open:${proto}`);
            steps.onopen(ws);
        };
        ws.onmessage = m => steps.onmessage(ws, m, log);
        ws.onerror = e => log.push(`error:${e}`);
        ws.onclose = (code, reason) => {
            log.push(`close:${code}:${reason}`);
            resolve(log);
        };
    }), 10000, url);
}

async function ws() {
    // text + a binary message bigger than the tx chunk (fragmented on the way out),
    // then the server closes on us with 4000
    const big = new Uint8Array(3000).map((_, i) => i & 0xff);
    let log = await wsSession(`ws://${HOST}:9002/echo?x=1`, {
        onopen: ws => ws.sendText('hello ws'),
        onmessage: (ws, m, log) => {
            if (typeof m === 'string') {
                log.push(`text:${m}`);
                ws.sendBinary(big.buffer);
            } else {
                const b = new Uint8Array(m);
                log.push(`bin:${b.length}:${b.every((v, i) => v === (i & 0xff))}`);
                ws.sendText('close-me');
            }
        },
    });
    check(`ws: ${log.join(' ')}`,
          log.join(' ') === 'open:echo text:hello ws bin:3000:true close:4000:server bye');

    // we close first; the server's reply carries the code but no reason
    log = await wsSession(`ws://${HOST}:9002/`, {
        onopen: ws => ws.close(1000, 'bye'),
        onmessage: () => {},
    });
    check(`ws: ${log.join(' ')}`, log.join(' ') === 'open:echo close:1000:');

    // server sends more than CONFIG_QJS_ESP_WS_MAX_MESSAGE
    log = await wsSession(`ws://${HOST}:9002/`, {
        onopen: ws => ws.sendText('huge'),
        onmessage: () => {},
    });
    check(`ws: ${log.join(' ')}`, log.join(' ') === 'open:echo error:message too big close:1006:');

    // nobody listening
    log = await wsSession(`ws://${HOST}:9009/`, { onopen: () => {}, onmessage: () => {} });
    check(`ws: ${log.join(' ')}`, log.join(' ') === 'error:connect failed close:1006:');
}

for (const t of [udp, tcp, ws]) {
    try {
        await t();
    } catch (e) {
        check(`${t.name}: ${e}`, false);
    }
}
console.log('all done');
