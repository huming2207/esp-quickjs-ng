import * as std from 'qjs:std';
import * as os from 'qjs:os';

console.log(`QuickJS-NG on ${os.platform}`);

// timers come from quickjs-libc, the FreeRTOS loop just sleeps until the next one
const t0 = performance.now();
await new Promise(resolve => os.setTimeout(resolve, 100));
console.log(`os.setTimeout(100) took ${(performance.now() - t0).toFixed(1)} ms`);

let ticks = 0;
await new Promise(resolve => {
    const id = os.setInterval(() => {
        if (++ticks === 3) {
            os.clearInterval(id);
            resolve();
        }
    }, 50);
});
console.log(`setInterval fired ${ticks} times`);

// os.sleep() blocks the JS task (nanosleep -> vTaskDelay)
const t1 = performance.now();
os.sleep(20);
console.log(`os.sleep(20) took ${(performance.now() - t1).toFixed(1)} ms`);

await os.sleepAsync(10);
console.log('os.sleepAsync ok');

// promise jobs run between events
const results = await Promise.all([1, 2, 3].map(async x => x * 2));
console.log('Promise.all:', JSON.stringify(results));

console.log('std.getenv(HOME):', std.getenv('HOME'));
console.log('cwd:', os.getcwd()[0]);

// rejected and never handled: the loop prints it instead of exit(1)
Promise.reject(new Error('nobody catches me'));

// rejected, then handled in the same tick: must stay quiet
const p = Promise.reject(new Error('caught'));
p.catch(() => {});

// a throwing timer gets printed, the loop keeps going
os.setTimeout(() => { throw new Error('thrown from a timer'); }, 10);
os.setTimeout(() => console.log('still alive after the throw'), 20);
