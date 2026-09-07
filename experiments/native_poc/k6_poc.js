// Mismos rangos y mismo patron que doFib/doPrimes en bench/k6/script.js,
// para que el numero sea comparable con el resto del banco de pruebas.
import http from 'k6/http';
import { Trend } from 'k6/metrics';

const BASE_URL = __ENV.BASE_URL || 'http://localhost:8096';
const VUS = parseInt(__ENV.VUS || '50', 10);
const DURATION = __ENV.DURATION || '20s';

export const options = {
    scenarios: {
        mixed: {
            executor: 'ramping-vus',
            startVUs: 0,
            stages: [
                { duration: '5s', target: VUS },
                { duration: DURATION, target: VUS },
                { duration: '3s', target: 0 },
            ],
        },
    },
    summaryTrendStats: ['avg', 'min', 'med', 'max', 'p(50)', 'p(90)', 'p(95)', 'p(99)'],
};

const tFib = new Trend('fib_ms', true);
const tPrimes = new Trend('primes_ms', true);

function randInt(min, max) { return Math.floor(Math.random() * (max - min + 1)) + min; }

export default function () {
    if (Math.random() < 0.5) {
        const n = randInt(20, 28);
        const res = http.get(`${BASE_URL}/compute/fib/${n}`, { tags: { name: 'fib' } });
        tFib.add(res.timings.duration);
    } else {
        const n = randInt(20000, 100000);
        const res = http.get(`${BASE_URL}/compute/primes/${n}`, { tags: { name: 'primes' } });
        tPrimes.add(res.timings.duration);
    }
}

export function handleSummary(data) {
    const lines = [];
    lines.push(`http_reqs: ${data.metrics.http_reqs.values.count}`);
    for (const key of ['fib_ms', 'primes_ms']) {
        const v = data.metrics[key].values;
        lines.push(`${key}: p50=${v['p(50)'].toFixed(2)}ms p90=${v['p(90)'].toFixed(2)}ms p95=${v['p(95)'].toFixed(2)}ms p99=${v['p(99)'].toFixed(2)}ms`);
    }
    return { stdout: lines.join('\n') + '\n', 'experiments/native_poc/result.json': JSON.stringify(data, null, 2) };
}
