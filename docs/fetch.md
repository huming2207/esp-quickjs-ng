# fetch and web globals

Enough of the browser API for code written for the web, or for Node, to mostly just run.

## Web globals

```c
qjs_esp_add_web_globals(ctx);   // after js_init_module_os()
```

Puts these on `globalThis`, skipping anything already there:

- `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval`, taken from `qjs:os`
- `TextEncoder`, `TextDecoder` (UTF-8 only)
- `AbortController`, `AbortSignal`, including `AbortSignal.timeout(ms)` and `AbortSignal.abort()`

It installs them by evaluating a bit of JS that imports `qjs:os`. So it needs `qjs:os`
registered, and a context that can eval.

## fetch

```c
qjs_esp_loop_t *loop = qjs_esp_loop_new(ctx);
qjs_esp_init_fetch(ctx);        // after the loop; adds the web globals too
```

```js
const r = await fetch('https://example.com/api', {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ hi: 1 }),
    signal: AbortSignal.timeout(5000),
    timeout: 10000,               // not standard: ms, for connect and each read
});
if (r.ok) console.log(await r.json());
```

You get `fetch()`, `Headers` and `Response`. A `Response` has `text()`, `json()`,
`arrayBuffer()`, `bytes()`, `clone()`, `Response.json()` and `Response.error()`.

## Not like a browser

- The body is buffered whole, at most `CONFIG_QJS_ESP_FETCH_MAX_BODY_KB` (512 KB, in PSRAM when
  there is some). No streams, so `Response.body` is `null`.
- Request bodies: a string, an `ArrayBuffer` or a view. Only http and https.
- `statusText` is always `''`. No CORS, credentials, cache or mode.
- `redirect: 'manual'` hands you the 3xx; `'error'` rejects on one. Redirects are followed by
  hand, so `redirected` and `url` are right.
- An abort rejects at once, but the request keeps running in the background and its result is
  dropped. `esp_http_client` can't be interrupted.
- https uses the certificate bundle (`MBEDTLS_CERTIFICATE_BUNDLE`).

## Cost

Each request is a task (`CONFIG_QJS_ESP_FETCH_TASK_STACK`, 8 KB; TLS needs most of it). At most
`CONFIG_QJS_ESP_FETCH_MAX_INFLIGHT` (4) run at once and the rest wait their turn. Freeing the loop
waits for running requests, up to their timeout.
