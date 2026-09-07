# Prueba de concepto: ¿compensa compilar a C++ nativo?

Objetivo: antes de comprometerse al refactor grande de `--native` (ver
`COMPILACION-NATIVA.md`), medir el número real. No es un transpilador — es
`fib()`/`cuenta_primos()` escritas a mano en C++, copiando exactamente la lógica de
`bench/lumen/app.lum`, con los mismos rangos de entrada que usa `bench/k6/script.js`.

Dos mediciones, a propósito separadas:

1. **`bench_direct`** — llama a las funciones en un bucle cerrado, sin HTTP de por medio.
   Esto aísla el coste de *ejecutar la lógica*, que es la pregunta que de verdad importa para
   decidir si vale la pena compilar Lumen Script.
2. **`server` + `k6_poc.js`** — las mismas funciones detrás de un servidor HTTP de sockets
   crudos, sin framework, sin keep-alive (una conexión TCP nueva por petición, un hilo de C++
   por conexión). Esto mide *cuánto añade el transporte* cuando el transporte es deliberadamente
   ingenuo — no es una comparación justa contra el motor HTTP de Lumen ni contra Gin como
   framework, es la cota superior de overhead de mi propio servidor de juguete.

## Resultado: coste de ejecutar la lógica (lo que responde a la pregunta)

| | fib (n=20-28) p50 | primes (n=20k-100k) p50 |
|---|---:|---:|
| VM de Lumen (medido vía HTTP, `bench/RESULTS.md`) | 20.09ms | 192.6ms |
| Gin / Go (medido vía HTTP, mismo banco de pruebas) | 0.21ms | 2.1ms |
| **C++ nativo, bucle cerrado sin HTTP (`bench_direct`)** | **0.036ms** | **1.86ms** |

**La VM tarda ~560x más que C++ nativo en `fib` y ~104x más en `primes`.** Y el número de C++
nativo no solo se acerca a Go — lo iguala en `primes` y lo mejora en `fib`, con la misma lógica,
el mismo compilador (GCC) que ya usa Lumen para compilarse a sí mismo. Esto es justo lo que
predice la tesis central de `COMPILACION-NATIVA.md`: con los tipos ya resueltos en compilación,
un bucle de Lumen Script compilado es indistinguible del mismo bucle escrito directamente en
C++ — no hay ninguna razón estructural para que sea más lento que Go.

## Resultado: con el servidor de sockets crudos del PoC (para que no se lea como más de lo que es)

| | fib p50 | primes p50 |
|---|---:|---:|
| Servidor de sockets del PoC (`server` + `k6_poc.js`) | 3.43ms | 7.31ms |
| `/health` del mismo servidor (línea base de overhead de transporte) | — | 0.36ms |

La diferencia entre estas dos tablas (0.036ms → 3.43ms en `fib`) es **enteramente mío**: una
conexión TCP nueva por petición y un `std::thread` por conexión son las dos decisiones más
ingenuas posibles para un servidor HTTP, tomadas a propósito para no escribir un framework en
esta prueba. El motor HTTP real de Lumen (`lumen::App`/`Router`, con su propio bucle de eventos
sobre `epoll`/`io_uring`) ya resuelve esto — es la misma razón por la que hoy, interpretado,
Lumen sirve `/health` en 0.05-0.08ms. Un handler compilado a nativo se registraría en ese mismo
motor exactamente igual que uno interpretado (§5 de `COMPILACION-NATIVA.md`), así que el coste
de transporte real que pagaría es el que Lumen ya paga hoy, no el de este servidor de juguete.

## Conclusión de la prueba

La tesis de `COMPILACION-NATIVA.md` queda validada con un número, no solo con la lectura del
perfil: **compilar el camino de CPU pura a C++ nativo cierra la distancia con Go, y lo hace con
margen** (104x-560x según el caso). No queda validado *cuánto* del rendimiento final del
`--native` real dependerá de la ingeniería del backend (representación de `Value`, refcounting,
generación desde el IR) — eso solo lo dirá implementarlo — pero la pregunta que esta prueba
tenía que contestar, "¿el techo de rendimiento está ahí si se compila de verdad?", tiene
respuesta: sí.

## Apéndice: se intentó también un arreglo barato en la VM (y no dio lo esperado)

Antes de escribir el plan de `--native`, el perfil de CPU señalaba
`std::vector<Value>::emplace_back` con un 14.4% del tiempo de hilo en la pila de operandos de
la VM (`stack_`, `include/lumen_script/vm.hpp`). La hipótesis barata: `stack_.reserve(32)` era
un valor arbitrario (`src/lumen_script/vm.cpp`, `VM::start()`) y `locals_`/`frames_` no
reservaban nada en absoluto, así que crecían por reasignación repetida en cada llamada. Se
subió `stack_` a 256, se añadió `locals_.reserve(256)` y `frames_.reserve(kMaxFrames)` (el tope
de recursión ya existía como constante, nunca se reservaba).

**Las 79 pruebas de `tests/run_tests.sh` siguen pasando** — el cambio es seguro. Pero medido en
la misma máquina, misma sesión, back-to-back (con el cambio vs. sin él, revertido con
`git stash`), la diferencia en `compute/primes`/`compute/fib` quedó **dentro del ruido**: no
hay una mejora medible. La razón, en retrospectiva, es la correcta: el símbolo `emplace_back`
que aparecía en el perfil no era mayoritariamente el coste de *reasignar* el vector — era el
coste de la propia llamada a `push_back` (comprobar capacidad, construir, incrementar tamaño)
en el camino rápido, que ocurre en **cada** instrucción de la VM que empuja un valor, con o sin
reasignación de por medio. Reservar capacidad evita las reasignaciones (que además eran pocas:
esta misma `VM` se reutiliza entre peticiones vía el `shared_vm` de `project.cpp`, así que solo
reasignaba en el arranque de cada hilo) pero no toca ese coste de fondo.

Esto no invalida la idea de tocar la VM — invalida la idea de que hubiera una victoria barata
ahí. El coste real es estructural: **cada operación de Lumen Script paga el precio de pasar por
`push`/`pop` sobre un `Value` boxeado y por el `switch` de despacho de opcodes**, y eso no se
arregla con un `reserve()`. Es exactamente la razón estructural que documenta `--native`: la
única forma de quitar ese coste es no pagarlo — compilar a código que no despacha opcodes ni
empuja `Value`s en absoluto. El cambio se deja en el árbol de trabajo (`src/lumen_script/vm.cpp`)
porque es correcto y no cuesta nada, pero **no se cuenta como una mejora de rendimiento
demostrada** — solo como higiene.

## Reproducir

```bash
cd experiments/native_poc
g++ -O2 -std=c++17 -o bench_direct bench_direct.cpp && ./bench_direct
g++ -O2 -std=c++17 -pthread -o server server.cpp
./server &
k6 run --env VUS=50 --env DURATION=20s k6_poc.js
```
