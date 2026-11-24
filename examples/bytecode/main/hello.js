import * as os from 'qjs:os';

function fib(n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

let line = '';
try {
    null.x;
} catch (e) {
    line = e.stack.split('\n')[0];
}
globalThis.result = `${os.platform} fib(15)=${fib(15)} ${line.trim()}`;
