// Mide fib()/cuenta_primos() en un bucle directo, sin HTTP de por medio, para
// aislar el coste de computo puro del coste del servidor de sockets del PoC.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

static int64_t fib(int64_t n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

static int64_t cuenta_primos(int64_t limite) {
    int64_t contador = 0;
    int64_t i = 2;
    while (i < limite) {
        bool es_primo = true;
        int64_t d = 2;
        while (d * d <= i) {
            if (i % d == 0) { es_primo = false; break; }
            d++;
        }
        if (es_primo) contador++;
        i++;
    }
    return contador;
}

template <typename F>
static void run(const char* label, int iters, F&& gen_and_call) {
    std::vector<double> samples;
    samples.reserve(iters);
    for (int i = 0; i < iters; i++) {
        auto t0 = std::chrono::high_resolution_clock::now();
        gen_and_call();
        auto t1 = std::chrono::high_resolution_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) { return samples[(size_t)(p * (samples.size() - 1))]; };
    printf("%-10s n=%d  p50=%.4fms  p90=%.4fms  p99=%.4fms  avg=%.4fms\n",
        label, iters, pct(0.50), pct(0.90), pct(0.99),
        [&]{ double s=0; for (auto v: samples) s+=v; return s/samples.size(); }());
}

int main() {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int64_t> fib_range(20, 28);
    std::uniform_int_distribution<int64_t> primes_range(20000, 100000);

    volatile int64_t sink = 0;
    run("fib", 20000, [&]{ sink += fib(fib_range(rng)); });
    run("primes", 2000, [&]{ sink += cuenta_primos(primes_range(rng)); });
    printf("(sink=%lld, solo para que el compilador no elimine las llamadas)\n", (long long)sink);
}
