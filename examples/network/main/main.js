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
                ws.sendBinary(new DataView(big.buffer));   // views work too
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
        onopen: ws => {
            try {
                ws.close(1006);     // reserved, not for clients
                log.push('1006 accepted');
            } catch (e) {
                // RangeError expected, carry on with a real close
            }
            ws.close(1000, 'bye');
            ws.close(1000, 'twice');    // no second CLOSE frame
        },
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

// things the review caught
async function regressions() {
    const throwsLike = (what, fn, pred) => {
        try {
            fn();
            check(`${what}: no throw`, false);
        } catch (e) {
            check(`${what}: ${e.name} ${e.message}`, pred(e));
        }
    };
    const u = new UDP();
    u.bind({ ip: '0.0.0.0', port: 9100 });
    u.close();
    throwsLike('udp startRecv after close', () => u.startRecv(), e => e.errno !== undefined);
    throwsLike('udp bind after close', () => u.bind({ ip: '0.0.0.0', port: 9101 }), e => e.errno !== undefined);
    throwsLike('udp port wrap', () => new UDP().send(enc('x'), { ip: HOST, port: 65536 + 80 }), e => e instanceof RangeError);

    class MyUDP extends UDP {}
    const mu = new MyUDP();
    check('udp subclass', mu instanceof MyUDP && mu instanceof UDP);
    mu.close();

    const { defines: D, uv_poll_event_bits: EV } = ps;
    const s = new ps.PosixSocket(D.AF_INET, D.SOCK_STREAM, 0);
    throwsLike('poll(DISCONNECT) alone', () => s.poll(EV.DISCONNECT, () => {}), e => e.errno !== undefined);
    s.close();

    // close() while CONNECTING, after the task already connected and parked in
    // select(): busy-wait so ev_open is still queued when close() runs. Used to hang.
    const log = await withTimeout(new Promise(resolve => {
        const ws = new WebSocket(`ws://${HOST}:9002/`, 'echo');
        const log = [];
        ws.onopen = () => log.push('open');
        ws.onerror = e => log.push(`error:${e}`);
        ws.onclose = code => { log.push(`close:${code}`); resolve(log); };
        const t = Date.now();
        while (Date.now() - t < 1500) {}
        ws.close();
    }), 8000, 'close while connecting');
    check(`ws close while connecting: ${log.join(' ')}`, log.join(' ') === 'error:closed before the connection was established close:1006');

    // used to report the 5th one straight away, mid-rejection
    const ps20 = Array.from({ length: 20 }, (_, i) => Promise.reject(new Error(`r${i}`)));
    await Promise.allSettled(ps20);
    check('20 rejections handled in the same turn (no "unhandled" above)', true);
}

// Idle must mean blocked: with a socket watched and a websocket open, nothing
// of ours should get CPU while JS waits on a timer. That's what lets tickless
// idle light-sleep the chip.
async function idle() {
    const sleep = ms => new Promise(r => os.setTimeout(r, ms));
    const u = new UDP();
    u.onrecv = () => {};
    u.startRecv();
    let ws;
    await withTimeout(new Promise((resolve, reject) => {
        ws = new WebSocket(`ws://${HOST}:9002/`, 'echo');
        ws.onopen = resolve;
        ws.onerror = e => reject(new Error(e));
    }), 5000, 'idle ws');
    await sleep(200);

    const a = taskTimes();
    const t0 = performance.now();
    await sleep(3000);
    const b = taskTimes();
    const wall = (performance.now() - t0) * 1000;
    const d = k => b[k] - a[k];
    console.log(`idle: io ${d('qjs_io')} us, ws ${d('qjs_ws')} us, js ${d('js')} us over ${(wall / 1e6).toFixed(2)} s`);
    check('idle: I/O and websocket tasks never woke', d('qjs_io') === 0 && d('qjs_ws') === 0);
    check('idle: JS task woke only for its timer', d('js') < 5000);

    const c = taskTimes();
    os.sleep(300);  // blocking sleep: must block in the scheduler, not spin
    const e = taskTimes();
    console.log(`os.sleep(300): js ${e.js - c.js} us`);
    check('os.sleep blocks', e.js - c.js < 30000);   // spinning would be ~300000; the rest is taskTimes() itself

    await new Promise(r => { ws.onclose = r; ws.close(1000); });
    u.close();
}

for (const t of [regressions, idle, udp, tcp, ws]) {
    try {
        await t();
    } catch (e) {
        check(`${t.name}: ${e}`, false);
    }
}
console.log('all done');
