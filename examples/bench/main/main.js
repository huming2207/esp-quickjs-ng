// Same workloads on every chip. cycles() wraps at 2^32, so each run stays
// well under that (~13 s at 320 MHz).
const cases = {
    'fib(20)': () => {
        const fib = n => (n < 2 ? n : fib(n - 1) + fib(n - 2));
        return fib(20);
    },
    'loop 200k': () => {
        let s = 0;
        for (let i = 0; i < 200000; i++) {
            s = (s + i * 7) | 0;
        }
        return s;
    },
    'string concat 5k': () => {
        let s = '';
        for (let i = 0; i < 5000; i++) {
            s += i.toString(36);
        }
        return s.length;
    },
    'JSON 200 objs': () => {
        const a = Array.from({ length: 200 }, (_, i) => ({ id: i, name: `item${i}`, tags: ['a', 'b'] }));
        return JSON.parse(JSON.stringify(a)).length;
    },
    'regexp 2k': () => {
        const re = /(\d+)-(\w+)/;
        let n = 0;
        for (let i = 0; i < 2000; i++) {
            n += re.exec(`${i}-abc`)[1].length;
        }
        return n;
    },
    'sort 10k': () => {
        const a = Array.from({ length: 10000 }, (_, i) => (i * 7919) % 10007);
        return a.sort((x, y) => x - y)[5000];
    },
    'Float64Array 50k': () => {
        const f = new Float64Array(50000);
        for (let i = 0; i < f.length; i++) {
            f[i] = Math.sqrt(i) * 1.5;
        }
        return f.reduce((x, y) => x + y, 0) | 0;
    },
};

for (const [name, fn] of Object.entries(cases)) {
    fn();   // warm up
    const t = performance.now();
    const c = cycles();
    const r = fn();
    const dc = (cycles() - c) >>> 0;
    const dt = performance.now() - t;
    console.log(`${name.padEnd(18)} ${dt.toFixed(1).padStart(8)} ms ${(dc / 1e6).toFixed(1).padStart(9)} Mcycles  (${r})`);
}
