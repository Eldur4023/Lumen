# Fuzzing

Dos arneses, sin libFuzzer: este toolchain es GCC, y `-fsanitize=fuzzer` es un
builtin de clang que GCC no tiene. El proyecto tampoco baja herramientas de la
red para compilar, así que instalar clang solo para esto habría sido una
dependencia nueva para un problema que se resuelve sin ella.

Lo que hay en su lugar, en `chaos.hpp`: sin guía por cobertura, pero cada caso
—una semilla válida mutada a bocados: bit-flips, bytes al azar, inserciones,
borrados, empalmes con otra semilla— corre en su propio proceso hijo. Un
cuelgue o un `abort()` de ASan/UBSan tumba solo ese hijo; la campaña sigue y
el caso que falló se vuelca a `/tmp/<nombre>_fallo_N.bin` para reproducirlo
aparte.

- **`fuzz_lenguaje`** — lexer, parser y checker de Lumen Script. Semillas: todos los
  `.lum` de un directorio (por defecto `tests/casos`). Cada caso se escribe a
  un fichero temporal y se compila con `lumen_script::compile()`, la misma función
  que usa `lumen --check` — se prueba el camino real.
- **`fuzz_http`** — el parser HTTP (llhttp de por medio) y el parser
  multipart, los dos en proceso, sin socket. `http_parser.hpp` es interno a
  `lumen` (vive en `src/`, no en `include/`); el arnés lo incluye directo,
  como ya hace `tests/marcadores.cpp` con el driver de postgres.

## Compilar

Aparte del build normal: para que ASan/UBSan sirvan de algo tienen que
instrumentar también `lumen_script`/`lumen`, no solo los dos `.cpp` del arnés, así
que van en los flags de la configuración entera, no en el target.

```bash
mkdir -p build-fuzz && cd build-fuzz
cmake -S .. -B . -DCMAKE_BUILD_TYPE=Debug -DLUMEN_FUZZ=ON -DLUMEN_JEMALLOC=OFF \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build . -j"$(nproc)" --target fuzz_lenguaje fuzz_http
```

## Correr

```bash
export ASAN_OPTIONS=abort_on_error=1:detect_leaks=1
export UBSAN_OPTIONS=abort_on_error=1:print_stacktrace=1

./fuzz_lenguaje ../tests/casos 100000   # semillas, iteraciones
./fuzz_http 100000                      # iteraciones (los dos objetivos internos)
```

`abort_on_error=1` es lo que permite que `chaos::correr()` distinga un caso
que falló de uno que no: sin él, ASan hace `exit(1)` en vez de mandarse una
señal, y `WIFSIGNALED` nunca se cumple.

A unos cientos de casos por segundo —el coste es el `fork()` por caso, no hay
forma de evitarlo sin guía por cobertura que decida qué casos vale la pena
correr—, 100.000 iteraciones tardan minutos, no segundos. Es fuzzing "tonto":
lo que compensa es partir de semillas reales, no la velocidad.

## Reproducir un fallo

```bash
xxd /tmp/lenguaje_fallo_1.bin        # o el nombre que haya volcado la campaña
cp /tmp/lenguaje_fallo_1.bin /tmp/caso.lum
./fuzz_lenguaje /tmp   1             # una sola iteracion, semilla = el propio caso
```

Para `fuzz_http`, el volcado es el buffer crudo que se le pasó a `feed()` o a
`parse_multipart()` — se puede releer a mano contra `chaos::correr` con una
sola semilla y una iteración, o simplemente inspeccionar los bytes.
