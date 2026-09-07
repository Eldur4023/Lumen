# Fuzzing

Two harnesses, without libFuzzer: this toolchain is GCC, and `-fsanitize=fuzzer`
is a clang builtin GCC does not have. The project does not download tools from
the network to build either, so installing clang just for this would have been a
new dependency for a problem that can be solved without one.

What is here instead, in `chaos.hpp`: no coverage guidance, but every case —a
valid seed mutated in bites: bit flips, random bytes, insertions, deletions,
splices with another seed— runs in its own child process. A hang or an
`abort()` from ASan/UBSan takes down only that child; the campaign carries on
and the failing case is dumped to `/tmp/<name>_fail_N.bin` so it can be
reproduced separately.

- **`fuzz_language`** — the Lumen Script lexer, parser and checker. Seeds: every
  `.lum` in a directory (`tests/cases` by default). Each case is written to a
  temporary file and compiled with `lumen_script::compile()`, the same function
  `lumen --check` uses — the real path is tested.
- **`fuzz_http`** — the HTTP parser (with llhttp involved) and the multipart
  parser, both in process, with no socket. `http_parser.hpp` is internal to
  `lumen` (it lives in `src/`, not in `include/`); the harness includes it
  directly, as `tests/placeholders.cpp` already does with the postgres driver.

## Building

Separate from the normal build: for ASan/UBSan to be worth anything they have to
instrument `lumen_script`/`lumen` too, not just the two harness `.cpp` files, so
they go in the flags for the whole configuration, not on the target.

```bash
mkdir -p build-fuzz && cd build-fuzz
cmake -S .. -B . -DCMAKE_BUILD_TYPE=Debug -DLUMEN_FUZZ=ON -DLUMEN_JEMALLOC=OFF \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build . -j"$(nproc)" --target fuzz_language fuzz_http
```

## Running

```bash
export ASAN_OPTIONS=abort_on_error=1:detect_leaks=1
export UBSAN_OPTIONS=abort_on_error=1:print_stacktrace=1

./fuzz_language ../tests/cases 100000   # seeds, iterations
./fuzz_http 100000                      # iterations (both internal targets)
```

`abort_on_error=1` is what lets `chaos::run()` tell a failing case from a
passing one: without it, ASan calls `exit(1)` instead of raising a signal, and
`WIFSIGNALED` is never true.

At a few hundred cases per second —the cost is the `fork()` per case, and there
is no way around it without coverage guidance to decide which cases are worth
running— 100,000 iterations take minutes, not seconds. This is "dumb" fuzzing:
what pays off is starting from real seeds, not the speed.

## Reproducing a failure

```bash
xxd /tmp/lenguaje_fail_1.bin        # or whatever name the campaign dumped
cp /tmp/lenguaje_fail_1.bin /tmp/case.lum
./fuzz_language /tmp   1             # a single iteration, seed = the case itself
```

For `fuzz_http`, the dump is the raw buffer that was handed to `feed()` or to
`parse_multipart()` — it can be replayed by hand against `chaos::run` with a
single seed and one iteration, or simply inspected byte by byte.
