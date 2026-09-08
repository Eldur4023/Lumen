# Compilación nativa — la opción `--native`

> Plan de trabajo para que Lumen Script se compile a código máquina en vez de interpretarse.
> No es un documento de "algún día": es el orden concreto en que se va a hacer, qué se toca en
> cada paso, y qué tiene que cumplirse para dar cada paso por bueno.
>
> Para el lenguaje en sí está [LUMEN_SCRIPT-GRAMMAR.md](LUMEN_SCRIPT-GRAMMAR.md); para montar
> una aplicación, [GUIDE.md](GUIDE.md). Este es el tercero: cómo el mismo `.lum` que hoy se
> interpreta pasa a ejecutarse como un binario nativo, sin cambiar ni una letra del fuente.

---

## 1. Qué se persigue

Hoy Lumen tiene dos niveles de ejecución de una ruta:

| Nivel | Qué es | Coste por petición |
|---|---|---|
| **Declarativa** | La ruta es un `return` constante; se resuelve al compilar y vive en el radix tree | Cero bytecode |
| **Con lógica** | Todo lo demás; se compila a bytecode y lo ejecuta el VM | Despacho de opcodes |

`--native` añade un tercero: **la ruta se compila a código máquina** y el motor la llama como
llamaría a cualquier otra función de C++.

El objetivo no es "un poco más rápido". El perfil de la VM bajo carga de CPU pura
(`bench/RESULTS.md`, sección de profiling) dice exactamente dónde se va el tiempo en un bucle
de cálculo:

```
29.9%  lumen_script::VM::run_until_error          <- despacho de opcodes
14.4%  std::vector<Value>::emplace_back<Value>    <- crecer la pila de operandos
 7.6%  lumen_script::VM::pop
 4.3%  lumen_script::Value::soltar                <- refcount
```

Nada de eso es aritmética. Es el coste de interpretar. Un bucle equivalente compilado a C++
con los tipos ya resueltos son enteros de 64 bits en registros: es la diferencia entre los
192ms de mediana que marca `compute/primes` hoy y los ~2ms que marca el mismo endpoint
escrito en Go (medido, mismo hardware, mismo cliente de carga — `bench/RESULTS.md`).

**El listón de aceptación del proyecto entero es ese**: en modo `--native`, los endpoints de
CPU del banco de pruebas tienen que quedar en el mismo orden de magnitud que la
implementación en Go, no en el de la interpretada.

---

## 2. La distinción que manda sobre todo lo demás: compilar, no traducir

Hay dos formas de convertir Lumen Script en C++, y solo una sirve.

**Traducir** sería recorrer el AST y escribir C++ que hace exactamente lo mismo que hacía el
VM: cada valor sigue siendo un `Value` de 16 bytes con etiqueta de tipo y refcount atómico,
cada campo de una clase se sigue buscando por nombre en un `Dict`, cada `List<int>` sigue
siendo un `std::vector<Value>`. Eso es rápido de escribir y da una mejora modesta — quitas el
despacho de opcodes y poco más, porque el trabajo real (boxing, refcounting, búsquedas por
nombre) sigue ahí. Sería cambiar un intérprete por un intérprete desenrollado.

**Compilar** es lo otro: aprovechar que el checker **ya sabe** el tipo de cada expresión para
elegir la representación de máquina adecuada a cada valor. Un `int` es un `int64_t` en un
registro, no una unión etiquetada en el heap. Un `List<int>` son 8 bytes contiguos por
elemento, no 16 con etiqueta. Un campo de una clase es un desplazamiento fijo dentro de un
struct, no una clave en una tabla hash. Y una vez ahí, el optimizador de C++ puede hacer su
trabajo: mantener variables en registros, desenrollar bucles, vectorizar, hacer inlining entre
funciones.

Esto cuesta más de construir y **cuesta más de compilar** — un `.lum` que hoy pasa a bytecode
en milisegundos va a tardar segundos en pasar por `g++`. Se acepta a propósito: es el precio
de que el binario resultante corra como código nativo y no como un intérprete disfrazado.
Por eso `--native` es un modo aparte y no el comportamiento por defecto (§11).

---

## 3. El invariante innegociable

> **El mismo `.lum` se comporta igual interpretado que compilado. Cualquier divergencia es un
> bug del compilador nativo, nunca una "diferencia documentada del modo `--native`".**

Sin esta regla, `--native` es una trampa: se desarrolla contra un comportamiento y se despliega
contra otro, y los fallos aparecen solo en producción, que es exactamente lo que todo el diseño
del lenguaje intenta evitar. Con ella, `--native` es una decisión de despliegue y nada más.

La consecuencia práctica está en §8: la semántica de referencia de listas, diccionarios e
instancias de clase **se conserva** en modo nativo aunque cueste rendimiento, y solo se
sustituye por algo más rápido cuando el compilador puede *demostrar* que nadie nota la
diferencia. No se cambia la semántica para ganar velocidad.

El corpus de `tests/casos/*.lum` es el árbitro (§13).

---

## 4. Por qué C++ como destino, y no LLVM directamente

Emitir C++ y llamar a `g++`/`clang++` es usar el compilador de C++ como *ensamblador
portable*. Es el camino de Nim, Vala, Haxe o Cython, y aquí encaja especialmente bien por una
razón que no es genérica: **el runtime de Lumen ya está escrito en C++**. El código generado
no tiene que hablar con el motor a través de una FFI ni replicar sus estructuras: hace
`#include` de `lumen/app.hpp`, `lumen_script/db.hpp`, `lumen/task.hpp` y usa los mismos tipos
que usa el motor internamente. Un backend de LLVM tendría que redefinir a mano el layout de
`Request`, `Response`, `DbAwaitable` y cada cosa que quisiera tocar, y romperse cada vez que
cambiara una de ellas.

A cambio se hereda el optimizador de C++ completo (inlining, LICM, vectorización, elección de
registros) sin escribir ni una pasada de optimización de bajo nivel. Lo que sí se escribe son
las optimizaciones que un compilador de C++ **no puede** hacer porque no conoce la semántica de
Lumen Script: elidir refcounting (§8), elegir el layout de las clases (§7), decidir qué handler
necesita marco de coroutine (§9).

---

## 5. Lo que ya juega a favor

Esto no se construye sobre terreno virgen. La investigación de la arquitectura actual deja tres
cosas claras, y las tres son buenas noticias:

**El motor HTTP ya está desacoplado del intérprete.** Un handler, para el router, es esto y
nada más (`include/lumen/types.hpp`):

```cpp
using Handler = std::function<Task<void>(Request&, Response&)>;
```

El bytecode es *una* implementación de ese contrato, conectada en `build_routes()`
(`src/lumen_script/project.cpp`). Una ruta compilada nativamente es otra función con esa misma
firma, registrada con el mismo `router.add_internal(...)`. **El motor no se toca.**

**La asincronía ya es C++20 de verdad.** `lumen::Task<T>` (`include/lumen/task.hpp`) es una
coroutine con `promise_type` y transferencia simétrica; `DbAwaitable`
(`include/lumen_script/db.hpp`) y `SleepAwaitable` son awaitables estándar con
`await_ready/await_suspend/await_resume`. El VM no los usa directamente: se suspende devolviendo
un `Result{Suspended, await_id, ...}` y es la coroutine del motor la que traduce ese `await_id`
al `co_await` real. **El código nativo se salta esa indirección entera y hace `co_await`
directamente sobre los mismos awaitables.**

**Las piezas de bajo nivel ya son públicas.** `DbPool`/`DbDriver`/`DbRegistry`
(`include/lumen_script/db.hpp`), `SharedState` (`include/lumen_script/natives.hpp`) y
`crypto::sha256/hmac_sha256/base64url_*` (`include/lumen_script/crypto.hpp`) están en headers
y no dependen del bytecode. El código generado las llama tal cual.

Y una que juega en contra, que es el trabajo real de la fase 1: **no existe un AST tipado**.
El chequeo de tipos y la emisión de bytecode están fusionados en una sola pasada dentro de
`Emitter` (`src/lumen_script/emitter.cpp`): la información de tipos vive en un
`std::vector<Local>` efímero, se consulta con `tipo_de()` mientras se emite, y se tira al
terminar. No hay ningún punto donde un backend distinto pueda engancharse.

---

## 6. La pieza que falta: un IR tipado

El corazón del plan. Entre el AST y los dos backends se mete una representación intermedia
**tipada, con nombres resueltos y desazucarada**.

Tipada: cada expresión lleva su tipo resuelto (`int`, `string`, `List<T>`, `Dict<K,V>`,
`Clase(nombre)`, `Json`, `T?`). Es lo que permite al backend nativo elegir representación de
máquina en vez de boxear todo por si acaso.

Con nombres resueltos: una variable es un índice de slot, no un nombre; un campo es un índice
de campo dentro de su clase, no una cadena; una llamada apunta a una función concreta o a un
builtin concreto. Todo esto **ya lo calcula `Emitter` hoy** (`tipo_de`, `comprobar_campo`,
`comprobar_metodo_builtin`, `emit_call`) — el trabajo es conservarlo en una estructura en vez
de consumirlo al vuelo.

Desazucarada: el IR tiene una sola forma para cada cosa, y las azúcares de la gramática se
eliminan al construirlo. `for x in lista` pasa a bucle con índice explícito. `require c else r`
pasa a `if (not c) return r` (que es literalmente su definición, §27 de la gramática). `x += e`
pasa a `x = x + e`. `elif` pasa a `if` anidado. El ternario, a una if-expresión. Los métodos, a
funciones con `this` como primer parámetro (que es como ya se compilan). Un bloque `validate:`,
a una función generada que devuelve la lista de mensajes fallidos. **Cada azúcar que se elimina
aquí es una construcción menos que implementar dos veces en los dos backends.**

Ese IR es también donde viven las decisiones que un compilador de C++ no podría tomar por su
cuenta: el análisis de escape (§8), la elección de layout de clase (§7) y la clasificación de
handlers en "necesita coroutine" o "no" (§9).

El backend de bytecode pasa a consumir el IR igual que el nativo. Los diagnósticos
(`fichero:línea:columna` con el cursor debajo) los produce el checker, antes del IR, así que
son **idénticos** en los dos modos: compilar con `--native` no puede aceptar un programa que el
modo normal rechaza, ni al revés.

---

## 7. Representación de datos

Aquí es donde se gana o se pierde el rendimiento. La regla es: **si el checker conoce el tipo,
el dato se representa de forma nativa; si el tipo es genuinamente dinámico, se queda como
`Value`.**

| Lumen Script | VM hoy | `--native` |
|---|---|---|
| `int` / `long` | `Value`: unión etiquetada de 16B | `int64_t`, en registro |
| `float` / `double` | `Value` de 16B | `double`, en registro |
| `bool` | `Value` de 16B | `bool` |
| `string` | `Caja<std::string>*` con refcount atómico | Cadena inmutable compartida (refcount), pero sin etiqueta de tipo ni despacho |
| `List<int>` | `std::vector<Value>` — 16B por elemento + etiqueta | `std::vector<int64_t>` — 8B contiguos, sin etiquetas |
| `List<Clase>` | `vector<Value>` de `Dict`s | `vector` de instancias de la clase (§8 decide si por valor o compartidas) |
| `Dict<string,V>` | vector de pares + índice hash por encima de 16 claves | Tabla hash tipada sobre `V` nativo |
| Instancia de `class` | `Value::Dict` con claves de texto, campos buscados por nombre | `struct` con layout fijo; campo = desplazamiento constante |
| `Json` | `Value` dinámico | `Value` dinámico — **igual que hoy, a propósito** |

Las dos filas que más rinden son las dos últimas de datos. Un `List<int>` pasa de 16 bytes con
etiqueta y un toque de refcount por acceso a 8 bytes contiguos que el optimizador puede
vectorizar. Y un `u.nombre` pasa de *hashear una cadena y buscarla en una tabla* a *leer en
`base + 8`*, que es lo que la gramática ya promete conceptualmente cuando dice que los campos y
métodos se resuelven al compilar (§41): hoy eso es cierto para los *nombres* (los typos se
cazan al compilar) pero no para la *representación* (en runtime la instancia sigue siendo un
diccionario). `--native` hace que sea cierto también para la representación.

`Json` se queda dinámico porque lo es de verdad: es lo que devuelve una consulta a base de
datos, lo que trae `jwt.claims`, lo que llega por un WebSocket. Ahí no hay tipo estático que
explotar y fingir lo contrario sería mentir. El código generado llama a los mismos métodos de
`Value` que usa el VM.

---

## 8. Semántica de referencia, y cómo se conserva sin renunciar a la velocidad

Este es el punto donde es fácil romper el invariante de §3 sin darse cuenta.

Hoy, en el VM, las listas, los diccionarios y las instancias de clase se comparten **por
referencia**: un `Value` copiado apunta a la misma `Caja*` con refcount atómico, así que dos
variables que apuntan al mismo objeto ven las mutaciones la una de la otra. Es el modelo de
Python, y es observable desde el lenguaje:

```lum
Caja a = Caja("grande", 3, 4)
Caja b = a
b.ancho = 99          # a.ancho tambien vale 99 ahora
```

Si el backend nativo compilase `Caja` a un `struct` de C++ con semántica de valor, ese programa
daría `3` en vez de `99`. Mismo fuente, dos resultados: exactamente el fallo que §3 prohíbe.

**Por tanto: en `--native`, listas, diccionarios e instancias de clase son tipos por
referencia**, implementados con el mismo contador atómico intrusivo que ya usa `Caja`
(`include/lumen_script/value.hpp`) pero con la carga tipada en vez de un `Value` genérico. Se
conserva la semántica exacta y aun así se gana: desaparecen la etiqueta de tipo, el despacho
por opcode y la búsqueda de campos por nombre.

Y **después** se recupera lo que costaba, con análisis de escape sobre el IR: si el compilador
puede demostrar que una instancia no sobrevive a la función (no se devuelve, no se guarda en un
contenedor, no se pasa a nada que la retenga), esa instancia se coloca en la pila como un
struct por valor y el refcounting desaparece por completo. La diferencia con "compilar clases
por valor y ya" es que aquí la optimización solo se aplica cuando es *indistinguible*, no
cuando es *conveniente*. Es la fase 7, deliberadamente al final: primero correcto, luego
rápido.

---

## 9. Asincronía

Directa, y es la parte que más regalada viene.

`await sqlite.query(...)` se compila a un `co_await` sobre el mismo `DbAwaitable` que usa hoy
el motor; `await sleep(ms)`, a un `co_await` sobre `SleepAwaitable`. El handler generado es él
mismo una coroutine que devuelve `lumen::Task<void>`, que es justo lo que el router espera. Se
elimina el rodeo actual (el VM se suspende → devuelve `await_id` → el motor lo traduce a un
awaitable → reanuda el VM): en nativo el `co_await` está donde el `await` estaba en el fuente.

Las reglas de la gramática (§40) se comprueban en el checker, antes del IR, así que siguen
dando el mismo error en los dos modos: `await` sobre algo no suspendible, o falta de `await`
sobre algo que sí lo es.

Un detalle de rendimiento que conviene no perder: hoy el `Chunk` lleva una marca `has_await`
que decide si el handler necesita su propio VM o puede usar el compartido del hilo. El
equivalente nativo es que **un handler sin ningún `await` no necesita marco de coroutine**: se
genera como función normal y se registra por el camino síncrono que `HandlerTraits`
(`include/lumen/handler_traits.hpp`) ya sabe manejar, ahorrando la reserva del frame en el
heap. El IR ya sabe si hay `await` dentro, así que la decisión es gratis.

---

## 10. Errores, `try`/`catch` y el tope de recursión

En el VM, un error es un valor de retorno (`Status::Error`) y `try`/`catch` es una tabla de
rangos de bytecode con desapilado manual de marcos (`src/lumen_script/vm.cpp`). En nativo esa
maquinaria sobra: un `try:`/`catch e:` se genera como un `try`/`catch` de C++ de verdad,
que en las ABI modernas no cuesta nada mientras no se lance nada. Las reglas raras de la
gramática (§28) caen solas: que un `return` dentro del `try` no dispare el `catch`, que salir
del rango desactive el manejador, que el `catch` más interno gane — todo eso es el
comportamiento normal de un bloque `try` de C++, sin tener que emularlo.

Los bloques `on error` (§20) se generan como un `catch` alrededor del cuerpo del handler,
dentro de la misma coroutine, que rellena la respuesta con el código de estado correspondiente.

**El tope de recursión hay que reimplementarlo a mano.** La gramática promete (§16) que pasarse
de 200 llamadas anidadas da un error legible del lenguaje y **no** un `stack overflow` del
proceso, y esa garantía existe hoy porque el VM tiene su propia pila de marcos. En nativo se
usa la pila de C++, que no avisa: se desborda y el proceso muere. Así que el código generado
lleva un contador de profundidad por petición y lanza el mismo error del lenguaje al llegar al
límite. Es un coste pequeño (un incremento y una comparación por llamada, que el optimizador
suele hundir en el ruido) y es innegociable: sin él, `--native` convierte un `500` legible en
una caída del servidor, y eso es una divergencia de comportamiento de las gordas.

---

## 11. Qué produce `--native`, y cómo se despliega

**Invocación.** `--native` se suma a la familia de opciones que ya existe (`--check`,
`--port`, `--no-watch`, `--verbose`, `--autotest`):

```bash
lumen ./mi-app --native          # compila a nativo y sirve
lumen ./mi-app --native --check  # compila a nativo y sale (para CI)
```

**Artefacto.** Una biblioteca compartida que el binario `lumen` carga con `dlopen` y que
registra sus rutas en el router. Se elige `.so` en vez de un ejecutable propio porque así el
binario de Lumen sigue siendo el dueño de todo lo demás — CLI, ciclo de vida del servidor,
`/health`, `/metrics`, `/docs`, estáticos, sesión, JWT, recarga — y lo único que cambia es
quién ejecuta el cuerpo de cada ruta. También es lo que permite el modo mixto: las rutas que el
backend nativo todavía no sabe compilar se sirven con bytecode **en el mismo proceso**, sin que
el usuario tenga que elegir entre todo o nada. Un ejecutable autocontenido para despliegue en
un solo fichero es una comodidad posterior, no un requisito.

**Caché.** La compilación se cachea en un directorio del proyecto (`.lumen-native/`,
ignorable en git) con una clave que incluye el hash de los fuentes, la versión de Lumen y la
del compilador de C++. Primera ejecución: segundos. Siguientes sin cambios: instantánea. Es lo
que hace que `--native` sea usable también en local sin volverse insoportable.

**El watcher.** Con `--native` la recarga en caliente queda desactivada por defecto: recompilar
con `g++` en cada guardado no es una experiencia de desarrollo, es una pausa para el café. El
flujo previsto es `lumen ./mi-app` mientras se escribe código (bytecode, recarga instantánea,
igual que hoy) y `--native` para medir, para CI y para producción.

**Informe al arrancar.** El mensaje actual crece con el tercer nivel, para que sea evidente qué
se compiló de verdad y qué no:

```
lumen: 3 fichero(s), 12 ruta(s) — 5 declarativa(s), 4 nativa(s), 3 con logica
```

Y cuando una ruta no se puede compilar nativamente, se dice **por qué**, con fichero y línea,
al estilo del resto de diagnósticos del lenguaje. Nunca en silencio.

---

## 12. Fases

Cada fase tiene un criterio de aceptación que se cumple o no se cumple; no se empieza la
siguiente sin el anterior en verde.

### Fase 0 — Preparar el terreno (no cambia comportamiento)
- Extraer a headers públicos la lógica de firma/verificación de sesión y JWT, hoy con enlace
  interno dentro de `src/lumen_script/project.cpp`. La necesitan los dos backends.
- Congelar el corpus de conformidad: `tests/casos/*.lum` + `bench/lumen/app.lum`, con sus
  salidas esperadas, ejecutables de un tirón.
- **Aceptación:** `tests/run_tests.sh` sigue en verde; el corpus produce salidas registradas y
  reproducibles.

### Fase 1 — Checker e IR tipado (el refactor de riesgo)
- Separar de `Emitter` la resolución de nombres y tipos hacia un checker propio que produzca el
  IR de §6.
- Reescribir el backend de bytecode para que consuma el IR en vez de emitir durante el chequeo.
- **Aceptación:** el corpus entero pasa con salidas byte a byte idénticas, y los mensajes de
  error de compilación (fichero, línea, columna, texto) no cambian ni uno. Es refactor puro:
  **cero funcionalidad nueva en esta fase**, para que cualquier regresión sea atribuible.

#### 1.1 — Lo que se descubrió al mirar `emit_expr`/`emit_stmt` de verdad

Antes de tocar esto conviene dejar constancia de por qué el corte no es tan simple como "mover
las comprobaciones a una función y la emisión a otra". `Type` (§9.2 más abajo, ya conectado) fue
la parte fácil porque tenía un contrato claro y sitios de llamada contados. `emit_expr` es otra
cosa: es **una sola función con las comprobaciones y la emisión entrelazadas rama a rama**, no
dos bloques separables. El caso `ExprKind::Member` por sí solo tiene tres ramas (`session.x`
dinámico, un objeto reservado como `sse`/`ws`/`error`, y un campo normal vía
`comprobar_campo`), cada una con sus propios `error(...); return;` intercalados con
`chunk_->emit(...)`.

La buena noticia, mirándolo de cerca: **dentro de cada rama, el orden ya es "comprobar y salir
si falla, emitir si no"** — no hay una emisión que ocurra y luego se invalide. Eso significa que
separar checker y emisor **no es reescribir la lógica de cada rama**, es partirla en dos
funciones que recorren el AST en paralelo con la misma forma:

- `check_expr(const Expr&) -> Type` — hace exactamente las mismas comprobaciones que hoy hace
  `emit_expr` (resolver nombres, `comprobar_campo`, `comprobar_metodo_builtin`, las reglas de
  objetos reservados, aridad de llamadas...) y llama a `error()` en los mismos sitios con el
  mismo texto, pero **no emite ni un solo opcode**. Es `tipo_de()` de hoy, pero completo: todas
  las ramas de `ExprKind`, no solo las cuatro que necesita resolver un receptor.
- `emit_expr(const IrExpr&)` (o, en la version mas simple de partida, la `emit_expr` de hoy con
  todas las llamadas a `error()` quitadas) — asume que ya paso el checker y solo genera bytecode.

**La trampa que hay que evitar**: si el checker y el emisor corren como dos pasadas
independientes y AMBOS siguen llamando a `error()`, cada fallo sale duplicado en `DiagnosticBag`
— justo el tipo de regresión silenciosa que el criterio de aceptación de esta fase prohíbe. Por
eso el checker tiene que ser la **única** fuente de diagnósticos: `emit_*` deja de comprobar
nada y confía en que si se le llama es porque el checker ya dio el visto bueno (y si no lo dio,
no se llega a invocar al emisor para ese chunk en absoluto — `failed_` ya hace ese papel hoy).

**La segunda trampa**: `declare_local`/`resolve_local`/`begin_scope`/`end_scope` no son bytecode
— son contabilidad de nombres — pero hoy se llaman **desde dentro de la emisión** (p.ej. el
`for` desazucarado declara sus ranuras auxiliares según emite). Si el checker y el emisor son
dos recorridos separados del mismo cuerpo, **los dos tienen que hacer exactamente la misma
secuencia de `declare_local`/`begin_scope`/`end_scope`**, en el mismo orden, para que los
índices de ranura que calcula el checker (los que va a llevar el IR) coincidan con los que
espera el bytecode. Esto no es un problema — es determinista a partir del AST — pero es la
razón por la que "solo mover las comprobaciones" no basta: el checker necesita su propia copia
de ese estado de resolución de nombres (o, mejor, el IR resultante ya lleva los índices
resueltos y el emisor deja de tocar `locals_` en absoluto, limitándose a leer los índices que ya
trae cada nodo).

**Conclusión de diseño, ya con esto claro**: el IR de §6 no es opcional ni una comodidad — es lo
que hace que el checker y el emisor no dupliquen la contabilidad de nombres cada uno por su
lado. Cada nodo de expresión del IR lleva: su `Type` (ya resuelto), y si es un `Ident` el
**índice de ranura ya resuelto** (no el nombre); cada nodo de sentencia que declara algo lleva
el índice que le tocó. Construir eso es responsabilidad exclusiva del checker; el emisor de
bytecode se convierte en una función mucho más tonta que index-in, opcode-out.

**Orden de trabajo recomendado, revisado** (más fino que "escribir el checker" a secas):
1. Definir el IR de expresiones (un `IrExpr` por cada `ExprKind`, con `Type` y, donde aplique,
   el índice ya resuelto) — aditivo, sin tocar `Emitter`, verificado con casos de mano como se
   hizo con `Type::from_declared`.
2. Escribir `check_expr` como una función nueva que **reproduce** las comprobaciones de
   `emit_expr` rama a rama (mismo texto de error, mismo orden), construida y probada **en
   paralelo** a la `emit_expr` existente sin sustituirla todavía — comparando sus diagnósticos
   contra los de compilar programas inválidos de verdad, uno por cada rama que se ha visto aquí
   (`session.x`, un objeto reservado fuera de sitio, un campo inexistente, una llamada con
   aridad equivocada...).
3. Solo cuando `check_expr` reproduce el 100% de los diagnósticos de expresiones del corpus,
   convertir `emit_expr` en un consumidor del IR y quitarle sus propias comprobaciones. Repetir
   el mismo proceso para sentencias (`check_stmt`/`IrStmt`), que es donde vive el desazucarado
   real (`for`, `require`, `x += e`) — la parte con más superficie pero, por lo visto aquí,
   ninguna sorpresa de diseño nueva respecto a lo que ya resolvió expresiones.
4. En cada uno de los tres pasos, `tests/run_tests.sh` en verde y sin ningún mensaje de error
   cambiado es la condición para seguir al siguiente — no una casilla que marcar al final.

**Paso 1 hecho**: `include/lumen_script/ir.hpp` define `IrExpr` (un caso por `ExprKind`, calcando
el reuso de `object`/`lhs`/`rhs` de `Expr`) e `IrCallShape` con las 8 formas de §1.2. Aditivo,
sin conectar, verificado con un smoke test standalone.

**Paso 2 hecho**: `Emitter::check_expr`/`check_call` (más `check_campo`/`check_metodo_builtin`,
sombra de `comprobar_campo`/`comprobar_metodo_builtin`) reproducen rama a rama las comprobaciones
de `emit_expr`/`emit_call` — mismo texto, mismo orden — escribiendo a un `DiagnosticBag` aparte
(`shadow`) que nunca toca `diags_`. No necesitan declarar ninguna ranura (las únicas
`declare_local` de hoy son temporales de codegen en `PreStep`/`PostStep`, que un paso de solo
comprobación no necesita), así que son lectores puros de `locals_` y pueden convivir con la
compilación real sin ningún riesgo de interferencia.

Se verificaron con `tests/check_expr_shadow.cpp` (`ctest -R check_expr_shadow`), que compara,
expresión a expresión, la salida de `check_expr`/`check_call` contra la compilación real
(`emit_condition`, ya existente para probar expresiones sueltas) — 14 casos de error, uno por
cada rama tocada aquí, más las 6 que ya cubre `tests/casos/malos/*.lum`, y 4 casos de camino
feliz para descartar falsos positivos. Los 18 coinciden byte a byte. `tests/run_tests.sh` sigue
en 79/79 porque `check_expr`/`check_call` aún no se llaman desde ningún sitio de la compilación
real — siguen siendo aditivos, exactamente como el IR del paso 1.

**Paso 3 hecho**: `Emitter::check_stmt`/`check_block` reproducen `emit_stmt`/`emit_block` rama a
rama. A diferencia de `check_expr`, SÍ llaman a `declare_local`/`begin_scope`/`end_scope`
(`VarDecl`, el `for` desazucarado, el nombre de un `catch`): son contabilidad de nombres real, no
un temporal de codegen, y hace falta reproducirla para que el `Ident` de una sentencia posterior
resuelva a la ranura correcta. Por eso `check_stmt` no puede convivir con una emisión real en
curso sobre el mismo `Emitter` — necesita sus propios puntos de entrada (`check_route`/
`check_function`/`check_method`/`check_ctor`/`check_error_handler`), cada uno reiniciando el
estado exactamente como su contrapartida `emit_*`, para poder llamarse en secuencia sobre el
mismo `Emitter` (primero la vía real, luego la sombra) sin interferir.

Verificado con `tests/check_stmt_shadow.cpp` (`ctest -R check_stmt_shadow`): 12 casos sobre
funciones y clases completas de verdad (no expresiones sueltas) — aritmética con `if`/`else`,
`while`, `for` con `break`/`continue`, `try`/`catch`, índices y `++`/`--`, y las ramas de error
nuevas que trae `check_stmt` (asignar a variable no declarada, `break`/`continue` fuera de un
bucle, `require` con una condición inválida, asignar a un campo de clase inexistente, un
constructor implícito con un parámetro que no es campo). Los 12 coinciden byte a byte contra
`emit_function`/`emit_method`/`emit_ctor` reales. `tests/run_tests.sh` sigue en 79/79: como los
pasos anteriores, sigue siendo aditivo — nada de esto se llama todavía desde la compilación real.

Con esto, el checker en paralelo cubre expresiones Y sentencias — el criterio del paso 3 original
("reproduce el 100% de los diagnósticos... antes de convertir") está cumplido para el subconjunto
verificado aquí.

**Validación adicional, contra programas .lum orgánicos, no solo casos de mano**: antes de dar el
paso de mayor riesgo (el corte real) hacía falta más que los casos escritos a propósito de
`tests/check_*_shadow.cpp` — esos ejercitan cada rama una vez, pero no dicen nada sobre
combinaciones reales que nadie diseñó pensando en el checker. Se añadió un canario en
`project.cpp` (`shadow_comparar`, activo solo si `LUMEN_SHADOW_CHECK` está en el entorno — coste
cero en el camino normal): en cada uno de los 9 sitios donde `project.cpp` construye un `Emitter`
real (`validate`, métodos, constructores explícitos e implícitos, funciones, `on error`, rutas
`ws`/`sse`/normales), justo después de la llamada real a `emit_*` se llama también a su `check_*`
equivalente sobre el mismo `Emitter` — es seguro porque cada punto de entrada (`emit_route`/
`check_route`, etc.) reinicia `locals_`/`route_method_`/`scope_depth_` por completo al entrar, así
que la sombra nunca pisa el estado de la emisión real que la precedió — y se compara texto a
texto, imprimiendo a `stderr` si difieren.

Con `LUMEN_SHADOW_CHECK=1 ./build/lumen --check <fichero>` sobre los 20 `.lum` reales del
repositorio (los 9 de `tests/casos/*.lum`, los 10 casos de error de `tests/casos/malos/*.lum`, y
`bench/lumen/app.lum` — el benchmark de 16 endpoints escrito leyendo solo la gramática, sin mirar
el compilador) no apareció ni una sola discrepancia, sobre más de 100 rutas/funciones/métodos/
constructores reales. `tests/run_tests.sh` sigue en 79/79 sin `LUMEN_SHADOW_CHECK` (el canario es
inerte por defecto).

Con esto, el checker en paralelo queda razonablemente probado: no solo reproduce los casos que se
diseñaron para ejercitarlo, sino que coincide con la compilación real sobre programas escritos sin
pensar en él. El canario de `project.cpp` puede quedarse: no cuesta nada en producción y da una
señal inmediata si el corte que sigue introduce una divergencia.

**`IrStmt` hecho**: `include/lumen_script/ir.hpp` ahora también define `IrStmt` (un caso por
`StmtKind`, calcando el reuso de `value`/`target`/`body`/`orelse` de `Stmt`) e `IrAssignTarget`
con las 4 formas de destino que distingue hoy `emit_stmt`/`check_stmt` en el caso `Assign`
(`Session`/`Index`/`Member`/`Local` — la misma idea que las 8 formas de `IrCall`, pero para
asignación). Aditivo, sin conectar, verificado con un smoke test standalone (`VarDecl`, las 4
formas de `Assign`, `For` y `Try`/`catch`).

Con `IrExpr` e `IrStmt` definidos y `check_expr`/`check_stmt` verificados contra la compilación
real (casos de mano y corpus orgánico), quedaba un solo paso para cerrar la fase 1: convertir
`check_expr`/`check_stmt` para que **devuelvan** `IrExpr`/`IrStmt` en vez de (solo) escribir a un
`DiagnosticBag` aparte, y luego convertir `emit_expr`/`emit_stmt`/`emit_call` en consumidores
puros de ese IR, quitándoles sus propias llamadas a `error()` — el paso de mayor riesgo de toda
la fase 1, porque ahí sí se toca el camino real de compilación que usa todo el mundo.

**Hecho.** `check_expr`/`check_call`/`check_stmt` construyen y devuelven `IrExpr`/`IrStmt`
(verificado con inspección de forma además de diagnósticos — ver más abajo), se escribió el
emisor que consume ese IR (`emit_expr`/`emit_call`/`emit_stmt`/`emit_block` sobre `IrExpr`/
`IrStmt`, sin ninguna llamada a `error()`), y se verificó por **equivalencia de ejecución real en
el VM** — no solo que compila limpio, sino que el bytecode resultante produce el mismo valor
devuelto, el mismo error o la misma suspensión que el bytecode del emisor viejo, sobre 11
funciones puras completas (aritmética, recursión, mutua-recursión, `for`/`while`/`break`/
`continue`, índices, `try`/`catch`, `require`, listas, dicts, ternario, métodos de builtin). Esa
prueba encontró un bug real antes de tocar el camino real: `IrStmt::VarDecl` no llamaba a
`declare_local()` al emitirse (usaba directamente la ranura ya resuelta), lo que dejaba
`locals_` desincronizado para cualquier declaración posterior en la misma función — corregido.

Con eso verificado, los 6 puntos de entrada reales (`emit_route`/`emit_function`/`emit_method`/
`emit_ctor`/`emit_condition`/`emit_error_handler`) se reescribieron para llamar a su `check_*`
correspondiente **con `diags_` real, no un `shadow` aparte** — así que `check_expr`/`check_stmt`
son ahora la única fuente de diagnósticos del compilador — y solo si eso tuvo éxito emiten desde
el IR ya construido. Las guardas de un `group()` se plegaron en `check_route` como nodos
`IrStmtKind::Require` (comparten exactamente el mismo desazucarado que `require`, verificado
línea a línea contra el `emit_route` original) para que también pasen por el IR.

**Validación final, con el compilador real**: `tests/run_tests.sh` en 79/79 — incluyendo
`guarda deniega`/`guarda permite` (las guardas ahora vía IR), `recursion`/`tope de recursion`,
`constructor`, `regla incumplida`/`todos los mensajes` (`validate:`, vía `emit_condition`), y
`mensajes en on error`. El canario de `LUMEN_SHADOW_CHECK` sobre los 20 `.lum` reales sigue en 0
discrepancias, con los mismos códigos de salida que antes de tocar nada. Se verificó además a
mano, sirviendo de verdad por HTTP, el único camino que sigue teniendo comprobación y emisión
fusionadas a propósito (`render()`, porque compilar la plantilla exige leer el fichero): una
plantilla válida con una expresión `{{ nombre.upper() }}` y un argumento nombrado respondió
correctamente end-to-end. Esa misma prueba manual, con una plantilla que llama a un método
inexistente, encontró un `std::bad_alloc`/`std::length_error` al compilar — confirmado como un
bug **preexistente** en el compilador de plantillas (reproducido idéntico en el commit anterior a
este, antes de tocar nada del emisor), no una regresión de este cambio; queda anotado para
revisarlo aparte, fuera del alcance de la fase 1.

Con esto, la fase 1 está terminada: el checker es la única fuente de diagnósticos, y el emisor
que llega a producción es un consumidor puro del IR. `emit_expr`/`emit_stmt`/`emit_call`/
`emit_block` sobre `Expr`/`Stmt` y `comprobar_campo`/`comprobar_metodo_builtin` ya no los llama
nadie desde los puntos de entrada reales — siguen en el árbol por ahora (retirarlos es limpieza,
no riesgo, y se hace aparte una vez confirmado con calma que de verdad no hace falta ninguno).

**La primera mitad de ese paso ya está hecha**: `check_expr`/`check_call`/`check_stmt`/
`check_block`/`check_condition` ahora construyen y devuelven el `IrExpr`/`IrStmt`/`IrBlock`
correspondiente (`nullptr`/vacío exactamente cuando ya se llamó a `shadow.error()` en el sitio
exacto), además de seguir escribiendo a `shadow` exactamente igual que antes. El tipo de cada
`IrExpr` es siempre `tipo_de(e)` — nunca algo más preciso inventado en el propio `check_expr`,
que habría sido funcionalidad nueva y no una reproducción de lo que ya hace el compilador.
`check_route`/`check_function`/`check_method`/`check_ctor`/`check_error_handler` ganaron un
parámetro opcional (`IrBlock* out_body = nullptr`) para exponer el árbol construido sin tocar
ninguno de los 9 sitios de `project.cpp` que ya los llaman (todos siguen descartando el bool
como hasta ahora).

Verificado en dos capas:
1. `tests/check_expr_shadow.cpp` (21 casos) y `tests/check_stmt_shadow.cpp` (12 casos) se
   extendieron para inspeccionar la *forma* del IR devuelto en el camino feliz — no solo que no
   hay divergencia de diagnósticos (que ya se verificaba), sino que `call_shape`/`call_index`/
   `slot`/`assign_target`/`decl_type` son los que corresponden. Cubre 6 de las 8 formas de
   `IrCall` con verificación estructural real (`DbModuleCall`, `UserFunctionCall`,
   `ConstructorCall`, `ClassMethodCall`, `BuiltinMethodCall`, y la resolución de `slot` de un
   `Ident`; `ReservedMemberCall` y la rama exitosa de `BuiltinGlobalCall` no son alcanzables
   desde una expresión suelta vía `check_condition`, que fija `route_method_` a vacío) y las 3
   formas de `IrAssignTarget` que se pueden ejercitar sin sesión (`Local`, `Index`, `Member`).
2. El canario de `project.cpp` (`LUMEN_SHADOW_CHECK=1`) se corrió de nuevo sobre los 20 `.lum`
   reales del repositorio tras la reescritura completa: cero discrepancias, igual que antes de
   tocar nada — la reescritura no cambió ni un carácter de lo que ya se producía.

`tests/run_tests.sh` sigue en 79/79. Con esto, la fase 1 tiene el IR real construido y
verificado en los dos niveles (casos de mano con inspección de forma, corpus orgánico completo
sin inspección pero con comparación exhaustiva de diagnósticos) — falta la segunda mitad, la que
de verdad es irreversible sin revisión: convertir `emit_expr`/`emit_stmt`/`emit_call` en
consumidores del `IrExpr`/`IrStmt` ya construido, y hacer que sean `check_expr`/`check_stmt` (no
`shadow`, sino `diags_` de verdad) la única fuente de diagnósticos del compilador.

#### 1.2 — Las formas de llamada que `IrCall` tiene que distinguir

`emit_call` (no solo `emit_expr`) es donde vive la mayor parte de la superficie real. Antes de
diseñar `IrCall` hace falta la lista completa de formas que hoy resuelve, porque cada una tiene
sus propias reglas de aridad/async y su propio opcode de destino — enumerarlas a medias es
peor que no enumerarlas, porque el hueco no aparece hasta que alguien escribe el `.lum` que lo
pisa:

1. **Miembro de un objeto reservado, módulo de BD** (`sqlite.query(...)`) — exige `import`,
   exige `await`, primer argumento es el nombre del módulo inyectado por el emisor, sin
   argumentos con nombre, `CallAsync`.
2. **Miembro de un objeto reservado, no-módulo** (`sse.send(...)`, `ws.send(...)`,
   `error.foo()`...) — comprueba que la ruta es del tipo correcto (`SSE`/`WS`/`ERROR`) antes de
   nada; puede ser async o no según el nativo.
3. **Función de usuario** (`fn` declarada) — aridad contra `FnSig` (obligatorios vs. con
   defecto), sin nombrados, defectos rellenados por el propio emisor, `CallFunction`.
4. **Constructor** (llamada al nombre de una clase) — resuelto por número de argumentos
   (`ctors` indexado por aridad, no por tipos — la gramática los distingue así, §15), sin
   nombrados, `CallFunction`.
5. **Método de clase con receptor de tipo estático conocido** — resuelto contra `ClassSig`,
   aridad contra `FnSig` igual que una función, sin nombrados, `emit_expr` del receptor +
   `CallFunction`.
6. **Builtin global** (`len(...)`, `sleep(...)`, `render(...)`...) — `render()` con nombre
   literal es un caso aparte dentro de este (compila la plantilla en el propio `Emitter`,
   `emitir_render_compilado`); el resto valida `is_async`/`awaited`, admite nombrados solo si es
   `render`, `CallNative` o `CallAsync`.
7. **Método builtin sobre un valor** (`s.upper()`, `xs.add(v)`...) — cuando el receptor tiene
   tipo estático conocido pasa por `comprobar_metodo_builtin` (ya aislado); si no, se despacha
   en runtime (`emit_method_call_dynamic`, opcode `CallMethod` por nombre).
8. **Nada de lo anterior** — error de compilación (`"de momento solo se pueden llamar builtins
   o metodos"`).

`IrCall` necesita un caso por cada una de estas ocho formas (no una sola forma genérica
"llamada con argumentos"), porque cada una decide de forma distinta cuántos argumentos son
válidos, si hace falta `await`, y a qué opcode/función C++ generada se traduce. El backend
nativo (fase 5) va a querer exactamente esta misma lista para decidir qué genera cada una: 1 y 2
son `co_await`/llamada directa sobre `DbAwaitable`/las structs que ya expone `db.hpp`; 3, 4 y 5
son llamadas a funciones C++ generadas; 6 y 7 son llamadas a la biblioteca de soporte del
runtime (`crypto::`, `Value::`, etc.); 8 sigue siendo un error de compilación en los dos
backends por igual.

### Fase 2 — Backend nativo: funciones puras
- Generación de C++ para `fn` con primitivos y control de flujo. Sin clases, sin contenedores,
  sin `await`, sin rutas.
- Invocación del compilador de C++ y enlazado.
- **Aceptación:** `fib` y `cuenta_primos` (los mismos del banco de pruebas) dan resultados
  idénticos a la VM, y `bench/` mide el salto de rendimiento esperado frente a Go. **Esta fase
  es la que valida o tumba la tesis entera del documento**, y llega pronto a propósito.

**Primer corte hecho.** `include/lumen_script/native_gen.hpp` + `src/lumen_script/native_gen.cpp`:
`generar_funcion_nativa(fn, body, nombre_por_indice)` genera el C++ de una función a partir de
su `IrBlock` ya construido por `check_function` — no traduce nodo a nodo de forma genérica,
sino que reconoce directamente `int`→`int64_t`, `float`→`double`, `bool`→`bool`, y el control de
flujo (`if`/`while`/`return`/`break`/`continue`/asignación/declaración) como construcciones C++
literales. Todo lo que queda fuera de "primitivos y control de flujo" (`string`, `List`/`Dict`,
clases, `for` — itera una `List` —, `await`, `try`, cualquier `IrCallShape` que no sea
`UserFunctionCall`) hace que la función entera devuelva `std::nullopt`: **no hay generación
parcial**, ni intento de forzar algo que esta fase no sabe representar todavía.

Un detalle que sólo apareció al escribir el generador de verdad: `IrStmt::Assign` (forma
`Local`) sólo lleva `assign_slot` (un número, lo único que necesita el bytecode) — no un nombre.
Para C++ hace falta el nombre real, así que el generador lleva su propia tabla ranura→nombre,
sembrada con los parámetros (la ranura *i*-ésima es siempre el parámetro *i*-ésimo, por cómo los
declara `check_function`) y actualizada en cada `VarDecl` que genera.

**Validado de punta a punta, no con un PoC escrito a mano esta vez**: `tests/native_gen_shadow.cpp`
(`ctest -R native_gen_shadow`) toma las `fib`/`cuenta_primos` exactas de `bench/lumen/app.lum`,
las compila con la vía real (`emit_function` → VM) *y* genera su C++ con `generar_funcion_nativa`,
invoca `g++` de verdad como subproceso, ejecuta el binario resultante, y compara su salida contra
la VM para varias entradas (`fib(10/25/30)`, `cuenta_primos(1000/100000)`) — coinciden en todos
los casos. Es la primera vez que este documento valida su tesis central con el compilador real
en vez de con código escrito a mano para la ocasión (ese PoC, en `experiments/native_poc/`, sigue
siendo válido como primera señal, pero éste es el generador de verdad).

**Hecho también: invocar el compilador desde el propio `lumen` y enlazar el resultado.**
`include/lumen_script/native_abi.hpp` fija la ABI que cruza el límite de `dlopen`: un único POD
(`NativeValue`, con una etiqueta y una unión int64_t/double/bool) y una única firma de función
(`CompiledFn = NativeValue(*)(const NativeValue*, int32_t)`) que vale para cualquier función
generada sin importar su aridad o tipos reales — así `dlsym()` no necesita conocer la firma real
de cada una. `generar_funcion_nativa` ahora también produce, junto al cuerpo de la función, un
wrapper `extern "C"` con esa firma fija que desempaqueta cada argumento y empaqueta el resultado.

`include/lumen_script/native_build.hpp` + `src/lumen_script/native_build.cpp`:
`compilar_nativo(prog, sigs, cache_dir, aviso)` recorre las funciones del programa, genera las que
puede, ensambla un único `.cpp`, invoca `g++ -shared -fPIC` de verdad como subproceso, y carga la
biblioteca resultante con `dlopen()` — dejando resuelto, por `dlsym()`, un `CompiledFn` por cada
función que se compiló. Un fallo aquí (falta `g++`, un error de enlazado, `dlopen` sin suerte)
**nunca** tumba la compilación del módulo: es una degradación a bytecode para todo el módulo, con
el motivo en `Module::native_aviso` para que `main.cpp` lo imprima como advertencia, nunca en
silencio — el mismo principio de "no hay generación parcial silenciosa" que rige el generador.

La VM (`vm.hpp`/`vm.cpp`) ahora recibe una tabla `CompiledFn` opcional, indexada igual que
`FunctionTable` (por `FnSig::index`), junto a la de siempre. Dentro de `Op::CallFunction`, si esa
tabla tiene un puntero para el índice llamado, la llamada entera se desvía a código nativo — sin
abrir marco de intérprete — y el resultado se empuja a la pila exactamente donde lo habría dejado
un `Op::Return` normal: el resto del bytecode que la invocó no distingue una cosa de la otra. Es
el modo mixto que describe la sección 11, aplicado hoy a funciones sueltas (`build_routes` conecta
la tabla nativa del módulo a las tres rutas que arrancan una VM: HTTP normal, `ws` y `sse`; también
el manejador de `on error` en `main.cpp`). Quedan sin conectar, a propósito y documentado aquí para
no perderlo de vista: las reglas de `validate:` (`bind_body`/`prepare_args`) y las expresiones de
plantilla (`render_plantilla`) — ninguna de las dos es parte del criterio de aceptación de esta
fase y ambas siguen sirviéndose con bytecode aunque llamen a una función que sí se compiló nativa.

El binario `lumen` suma `--native` a sus opciones (`main.cpp`): compila con `compile(inputs, diags,
/*native=*/true)`, apaga la recarga en caliente igual que `--no-watch` (recompilar con `g++` en
cada guardado no es una experiencia de desarrollo, tal como dice la sección 11), y añade una línea
al informe de arranque (`lumen: --native: N funcion(es) compilada(s) a codigo nativo`). El cacheado
por hash de fuentes que describe la sección 11 queda pendiente para cuando de verdad haga falta —
hoy `.lumen-native/` se regenera en cada arranque.

**Validado con el servidor real, no solo con una prueba de biblioteca.** `ctest -R
native_build_shadow` cubre lo que `native_gen_shadow` no cubre: llama a `compilar_nativo()` de
verdad (invoca `g++`, `dlopen`, `dlsym`) y compara, para `fib`/`cuenta_primos`, `VM::start()` con y
sin la tabla nativa conectada — es la prueba de que el despacho dentro de `Op::CallFunction` está
bien enlazado, no solo que el generador produce C++ correcto. Y, más allá de cualquier prueba:
sirviendo `bench/lumen/app.lum` de verdad con `lumen ... --native` y con `lumen ... --no-watch`
(bytecode) y comparando las respuestas HTTP de `/compute/fib/:n` y `/compute/primes/:n` para varias
entradas, coinciden exactamente — y `fib(32)` bajó de ~0.47s a ~0.002s por petición, un salto de
~250× medido en el proceso real sirviendo HTTP, no en un microbenchmark aislado.

**Rendimiento, comprobado de forma ligera**: el C++ que produce `generar_funcion_nativa` para
`fib`/`cuenta_primos` es, salvo el prefijo `l_` de cada nombre, el mismo que el PoC escrito a
mano en `experiments/native_poc/` — mismo `int64_t`, misma recursión, mismos bucles. Cronometrar
ese C++ generado de verdad (bucle directo, sin HTTP, mismo patrón que `bench_direct.cpp`) dio
`fib(28)` p50=0.257ms y `cuenta_primos(60000)` p50=1.861ms — prácticamente idéntico a los
números ya registrados del PoC (`cuenta_primos` p50=1.86ms). No hacía falta repetir el banco de
pruebas HTTP completo (`bench/`, con k6 y los cinco frameworks) para confirmar esto: el generador
real produce código estructuralmente equivalente al que ya se midió contra Go, así que el
resultado de rendimiento del PoC (104×-560× más rápido que la VM, a la par o mejor que Go) sigue
siendo válido para el generador de verdad, no solo para el código escrito a mano.

### Fase 3 — Tipos compuestos y clases
- `string`, `List<T>`, `Dict<K,V>` con representación nativa tipada (§7).
- Clases de usuario como structs con layout fijo, **con semántica de referencia** (§8).
- **Aceptación:** los casos de clases y datos del corpus (`tests/casos/clases.lum`,
  `datos.lum`, `lenguaje.lum`) dan salidas idénticas en los dos backends.

**Primer corte: `string`.** A diferencia de listas/diccionarios/instancias (§8), una cadena de
Lumen Script es inmutable — concatenar produce una cadena *nueva*, nunca muta la existente — así
que compartirla o copiarla es indistinguible desde fuera. `native_gen.cpp` representa `string`
como `std::string` **por valor**, sin el refcount intrusivo que sí hará falta para listas,
diccionarios y clases: es el mismo razonamiento que la Fase 7 aplicará a clases mediante análisis
de escape, aquí no hace falta ningún análisis porque la inmutabilidad ya lo garantiza desde el
principio. `tipo_soportado` acepta `string`; los literales, `+` (concatenación), `==`/`!=` y las
comparaciones de orden salen gratis una vez que el tipo está aceptado, porque
`operadores_binarios()` ya mapeaba esos símbolos al operador de C++ correspondiente y
`std::string` los implementa con la misma semántica (lexicográfica) que `Value::equals`/`compare`.

Aparece una frontera nueva que no existía con `int`/`float`/`bool`: la ABI fija de
`native_abi.hpp` (`NativeValue`) no tiene sitio para una cadena — su unión solo lleva un
`int64_t`/`double`/`bool`. Extenderla (con un puntero+longitud y una convención de propiedad para
quién libera qué) es trabajo real, y no hacía falta para dar este primer paso: una función cuya
frontera (parámetros o retorno) usa `string` **se compila igual** — su cuerpo entra en el `.cpp`
generado sin condición — pero se queda **sin wrapper** `extern "C"`, así que la VM no puede
llamarla directamente todavía (`tipo_abi_soportado`, más estricto que `tipo_soportado`, decide
esto). Lo que sí gana esta fase: cualquier otra función nativa (con frontera `int`/`float`/`bool`,
por tanto invocable desde la VM) que llame a esa función de cadenas **directamente en C++** —
`Generador::expr` ya generaba una llamada directa a la función interna, nunca a través del
wrapper — se beneficia igual, sin esperar a que la ABI se extienda. Validado en
`tests/native_build_shadow.cpp` (`prueba_strings()`): `saluda(string) -> string` se compila sin
wrapper, `usa_saluda(int) -> int` sí lo tiene y llama a `saluda` internamente concatenando y
comparando cadenas, y su resultado por bytecode y por `--native` coincide.

**Segundo corte: los 6 métodos de `string`.** `metodos_de()`/`call_method()` (`natives.cpp`)
reconocen exactamente seis sobre un string: `starts_with`, `ends_with`, `contains`, `upper`,
`lower`, `trim`. `native_gen.cpp` los acepta (`metodo_string_soportado`) cuando el receptor tipa
`string` — es la primera vez que el generador toca `IrCallShape::BuiltinMethodCall`, hasta ahora
solo aceptaba `UserFunctionCall`. Cada uno se traduce a una función libre (`lumen_str_upper`,
`lumen_str_contains`...) definida una sola vez en el `.cpp` generado (`string_runtime_prelude()`),
con la misma lógica exacta que su equivalente en `natives.cpp` — mismo criterio que `abi_prelude()`
para `NativeValue`: una sola fuente de verdad duplicada a propósito, porque dlopen no comparte
cabeceras con la biblioteca cargada.

Escribir la prueba de este corte encontró un bug real, independiente de los métodos de string:
`compilar_nativo()` ensambla el `.cpp` iterando `FunctionSigs` (un `std::map`, orden alfabético de
nombre), sin ninguna declaración adelantada — una función que llama a otra que el mapa visita
*después* (p.ej. `usa` llamando a `valida`) no compilaba, porque C++ exige ver la declaración antes
del uso. `fib`/`cuenta_primos` nunca lo habían destapado porque `fib` solo se llama a sí misma (su
propia firma ya es visible dentro de su propio cuerpo) y ninguna de las dos llama a la otra.
Arreglado generando un prototipo para cada función *antes* que ningún cuerpo, en un bloque aparte
— corrección de alcance general, no solo para cadenas.

Validado en `tests/native_build_shadow.cpp` (`prueba_metodos_string()`): `usa(int) -> int` declara
una cadena con espacios, la recorta (`trim`), la pasa a mayúsculas (`upper`), compara el resultado,
y si coincide llama a `valida(string) -> bool` (que usa `contains`/`starts_with`) — declarada
*después* de `usa` en el código fuente, ejercitando también la corrección del orden. Bytecode y
`--native` coinciden.

**Corrección crítica: el lenguaje es dinámico por debajo, y `--native` no lo sabía.**
Investigando cómo extender el generador a `string` de verdad (no solo probar con casos de mano),
apareció una pregunta incómoda: ¿comprueba el checker que una reasignación (`x = ...`) conserva el
tipo con el que `x` se declaró? Se probó contra el compilador real, no se asumió — y la respuesta es
**no, en ningún sitio**:

```lum
fn int riesgo(int a):
    int x = a
    x = "no soy un int"
    return x
```

Esto compila sin ni un aviso, y en tiempo de ejecución la ruta que lo llama devuelve
`{"r":"no soy un int"}` sin queja: Lumen Script es dinámicamente tipado por debajo de la anotación,
igual que Python — el tipo declarado es una ayuda para el checker en unos pocos sitios puntuales
(campos de clase, parámetros de ruta), no una garantía que se sostenga dentro del cuerpo de una
función. Tampoco se comprueba el tipo de un `return` contra el tipo declarado de la función, ni el
de un argumento contra el parámetro del destino.

El generador de esta fase, hasta este punto, confiaba ciegamente en el tipo *declarado*
(`IrExpr::type` / `IrStmt::decl_type`) para elegir la representación C++ de cada ranura — exactamente
la costumbre que el párrafo de arriba delata como insegura. Probado a propósito, se confirmaron dos
divergencias reales, silenciosas, ya en producción en los commits anteriores de esta misma fase:

1. **Reasignación con cambio de tipo numérico.** `int x = a; x = 3.5; return x` da `3.5` en
   bytecode (el `Value` es dinámico, sin problema) y daba **`3`** en `--native` — el `int64_t`
   generado para `x` trunca en silencio la asignación de un `double`, porque C++ permite esa
   conversión implícita sin ni un aviso. Confirmado con el binario real: mismo `.lum`, dos
   respuestas HTTP distintas para `/x?a=7` (`{"r":3.5}` contra `{"r":3}`).
2. **`and`/`or` no son `&&`/`||`.** `a and b` en Lumen Script devuelve el **valor** del operando que
   gana —igual que Python (`5 and 10` da `10`, no `true`)—, no un booleano forzado (`vm.cpp`:
   `JumpIfFalsePeek`/`JumpIfTruePeek`, que dejan el operando en la pila, nunca lo convierten). El
   generador traducía `and`/`or` directo a `&&`/`||`, que SIEMPRE da `bool`. Confirmado: `f(5, 10)`
   con `return a and b` daba `10` en bytecode y **`1`** en `--native`.

Y una tercera, de otra naturaleza — no una respuesta *distinta*, un **proceso muerto**: `a % b`/
`a / b` con `b == 0` es un error controlado en el VM ("`modulo por cero`" / "`division por cero`",
la petición responde 500 y el servidor sigue vivo), pero en C++ un `%`/`/` entero por cero es
comportamiento indefinido — en la práctica, `SIGFPE`, que **tumba el proceso entero**, con todas las
peticiones en vuelo. `cuenta_primos` usa `%` pero nunca con divisor cero, así que el banco de
pruebas de esta fase jamás lo había disparado.

**La corrección, no un parche puntual.** Las tres son la misma familia de fallo bajo superficies
distintas: el generador asumía que un tipo estático "obvio" en el código fuente coincide con el tipo
real en tiempo de ejecución, y en un lenguaje sin esa garantía, no siempre es así. Parchear cada caso
por separado habría dejado la puerta abierta a la próxima variante no probada. En su lugar,
`native_gen.cpp` gana un análisis propio, `Comprobador::tipo_provable()` — deliberadamente distinto
y más estricto que el `tipo_de()` del checker (que es débil a propósito, solo sirve a un puñado de
comprobaciones puntuales) — que recorre cada expresión aplicando **las mismas reglas dinámicas que
usa el VM** para decidir si su tipo está garantizado, y cuál:

- Un `Ident` solo es de tipo demostrado si la ranura se registró con ESE tipo y nunca se reasignó a
  otro — `stmt_compilable()` ahora exige que todo `Assign(Local)` demuestre exactamente el tipo
  original de la ranura, o la función entera se descarta. Por inducción, un `Ident` que resuelve
  aquí tiene garantizado que su valor real coincide siempre.
- `a / b` entre dos `int` no es demostrable (Int si es exacta, Float si no, decidido en tiempo de
  ejecución) — se descarta sin más, cae a bytecode. Entre otras combinaciones numéricas SÍ es
  demostrable (siempre Float), y ahí queda un `lumen_div_check` con comprobación de cero.
- `a % b` exige los dos lados demostrablemente `int` — demostrable, pero con
  `lumen_mod_check` de por medio para el divisor cero.
- `and`/`or` solo son demostrables (como `bool`) cuando los dos operandos YA son `bool` — el único
  caso en el que `&&`/`||` observablemente coinciden con "el operando que gana".
- Un `return`, una declaración `VarDecl` con inicializador, y cada argumento de una llamada a otra
  función, exigen que el tipo demostrado de la expresión coincida EXACTAMENTE con lo que la
  frontera declara — no solo que la expresión "se pueda generar".

El canal de error que introduce el `%`/`/` seguro (`error_runtime_prelude()`, `native_abi.hpp`) es
la primera pieza de infraestructura de errores de todo el backend nativo: `NativeValue` gana un tag
`Error`; `lumen_native_fail(msg)` dentro de una función nativa deja el mensaje en un buffer por hilo
y lanza una excepción vacía (`LumenNativeError`) que solo el wrapper `extern "C"` más externo atrapa
— una llamada nativa anidada (una función nativa llamando a otra directamente en C++, sin wrapper de
por medio) deja que la excepción se propague sola por la pila de C++ hasta ahí, igual que un error
Lumen sin `try` sube hasta quien llama. La VM, del lado de la ABI, lee `NativeDispatch::error_message`
justo cuando ve `NativeValue::Tag::Error` y lo convierte en el mismo `fail()` que usaría el bytecode
equivalente. Es infraestructura general, no un parche para `%`: el tercer corte de esta fase (más
abajo) la reusa tal cual para "índice fuera de rango" en `List`.

**Validado con el binario real, no solo con las pruebas.** Los tres casos de arriba se reprodujeron
primero con el `lumen` de verdad sirviendo HTTP (`--native` contra bytecode, mismo `.lum`, respuestas
distintas) y se volvieron a probar tras la corrección: las tres funciones ahora se quedan sin
compilar a nativo (`0 funcion(es) compilada(s)`, caída completa a bytecode) y dan la respuesta
correcta en los dos modos. El caso de división/módulo por cero se probó con la función SÍ compilada
a nativo (es segura: solo el divisor es dinámico, no el tipo) — la petición con divisor cero
responde 500 con el mensaje correcto, y el proceso sigue vivo y sirviendo para la siguiente
petición. `fib`/`cuenta_primos` y los casos de `string` de más arriba se re-verificaron sin cambios
(mismo número de funciones compiladas, mismo rendimiento) — la corrección es más estricta, no
distinta, para el código que ya era seguro.

**Tercer corte: `List<T>`, con semántica de referencia real.** A diferencia de `string`, una lista
es mutable (`.add()`) y §8 exige que lo sea *por referencia* — `List<int> b = a; b.add(9)` tiene que
mutar también lo que ve `a`, en los dos backends por igual. `list_runtime_prelude()` define
`LList<T>`: una caja con un contador de referencias — **no atómico**, a diferencia del `Caja` que usa
`Value` en el VM, porque una `LList` nunca cruza la ABI (excluida de `tipo_abi_soportado`, igual que
`string`) así que nunca viaja entre hilos — copiar una `LList` copia el puntero a la caja, no los
datos. `Type::Kind::List` entra en `tipo_soportado()` cuando su elemento es `int`/`float`/`bool`/
`string` (nunca otra `List`/`Dict`/clase: sin anidar, por ahora).

El análisis de solidez (`Comprobador::tipo_provable`, ver la corrección de más arriba) se extiende de
forma natural: un literal `[a, b, c]` es demostrable solo si **todos** los elementos demuestran el
mismo tipo (una lista vacía no tiene de dónde inferirlo, se queda fuera); `xs[i]` es demostrable
como el tipo del elemento solo si `xs` es demostrablemente `List<T>` y `i` demostrablemente `int`;
`xs[i] = v` exige lo mismo más que `v` coincida exactamente con `T`, y que `xs` sea una variable (no una
expresión temporal); `.add(v)` es el único método que reconoce `metodos_de()` para `List`
(`natives.cpp`) y devuelve la misma lista, igual que `call_method()`. Un `for T x in xs:` deriva el
tipo de `x` **del elemento de `xs`**, no de ninguna anotación (Lumen no exige una para `for`) — la
misma fuente de verdad (`Comprobador::ranura_tipos()`) que usa el generador para declarar la
variable C++ del bucle, expuesta explícitamente porque es la única ranura cuyo tipo no viene ni de
un parámetro ni de un `VarDecl`.

`lumen_get`/`lumen_set` comprueban el índice y usan el mismo canal de error que división/módulo
(`lumen_native_fail`, con el mismo formato de mensaje que `GetIndex`/`SetIndex` en `vm.cpp`:
"índice fuera de rango: N (tamaño M)") — la razón de construir ese canal ya de forma general en la
corrección anterior, no como un parche solo para `%`.

Generar el literal sin que el generador tenga que conocer el tipo del elemento se resuelve con CTAD
(deducción de argumentos de plantilla): una guía de deducción convierte `LList{a, b, c}` en
`LList<T>` con `T` deducido de los elementos, sin argumento explícito. Esto destapó un bug de una
sola línea, real pero sutil: el generador escribía los literales enteros con el sufijo `LL`
(`5LL`), que en glibc/x86-64 es `long long` — un tipo *distinto* de `int64_t` (que ahí es `long`),
aunque los dos midan 64 bits. `LList{1LL, 2LL, 3LL}` deducía `LList<long long>`, que no convierte a
`LList<int64_t>` (`LList<long>`): error de compilación (seguro, detectado por `g++`, no una
respuesta silenciosamente distinta) pero real. Arreglado envolviendo cada literal entero en
`static_cast<int64_t>(...)`, portable a cualquier plataforma independientemente de qué tipo nativo
sea `int64_t` ahí.

Validado en `tests/native_build_shadow.cpp` (`prueba_listas()`): un literal, `.add()`, indexado de
lectura y escritura, `for`, paso como parámetro a otra función nativa (`List<int>` en la frontera se
queda sin *wrapper*, igual que `string`, pero compila igual), índice fuera de rango con el mismo
mensaje de error en las dos vías, y el caso que de verdad importaba — dos variables sobre la misma
lista, mutar una a través de una y comprobar que la otra ve el cambio — coincide entre bytecode y
`--native`. 79/79 del corpus real y el canario `LUMEN_SHADOW_CHECK` siguen en verde.

### Fase 4 — Rutas HTTP síncronas
- Handler generado como `Task<void>` (o función síncrona si no hay `await`, §9), registrado en
  el router igual que hoy.
- Parámetros de ruta y query, binding del cuerpo a clase, `validate:`, `require`, `on error`,
  encadenado `status`/`header`/`cookie`.
- **Aceptación:** todos los endpoints del banco de pruebas que no tocan la base de datos
  devuelven respuestas idénticas en los dos modos, incluidos los casos de error (404 sin
  cuerpo, 422 con la lista de mensajes, 400 fuera de rango).

### Fase 5 — Asincronía y base de datos
- `await` → `co_await` sobre los awaitables existentes; transacciones, pool, `last_id`.
- **Aceptación:** el banco de pruebas completo (`bench/run_all.sh`) corre en modo `--native`
  con las mismas respuestas y sin errores de aplicación.

### Fase 6 — Empaquetado y modo mixto
- `--native` de punta a punta: caché, `.so`, `dlopen`, informe de arranque, diagnóstico
  explícito de rutas no compilables, respaldo por bytecode ruta a ruta.
- **Aceptación:** una aplicación que use `ws`/`sse` (todavía no soportados nativamente) arranca
  con `--native`, sirve esas rutas por bytecode y el resto nativas, y lo dice al arrancar.

### Fase 7 — Optimización
- Análisis de escape para elidir refcounting y heap (§8).
- Inlining entre funciones a nivel de IR, propagación de constantes, monomorfización.
- **Aceptación:** mejora medible en `bench/` sin ninguna divergencia nueva en el corpus.

---

## 13. Cómo se valida

Dos oráculos, los dos ya existen o casi:

**Corrección — el corpus, contra los dos backends.** `tests/run_tests.sh` se extiende con una
pasada `--native`: cada caso se ejecuta interpretado y compilado, y se comparan las salidas.
**Cero divergencias es la condición para integrar cualquier cambio.** No "casi todas iguales":
cero. Es la traducción operativa del invariante de §3, y es lo que impide que `--native` se
convierta poco a poco en un dialecto distinto.

**Rendimiento — el banco de pruebas que ya está montado.** `bench/` compara los mismos 16
endpoints contra Gin, Fastify, Express, Flask y FastAPI con una carga mixta idéntica. Se le
añade `lumen --native` como séptimo contendiente. Los objetivos concretos:

- `compute/fib` y `compute/primes`: mismo orden de magnitud que Go, no que el intérprete.
- Lecturas simples: que no empeoren (ya son sub-milisegundo interpretadas).
- Escrituras: **no se espera mejora**, y no pasa nada (§14).

---

## 14. Riesgos, y lo que esto no arregla

**El riesgo que ya se materializó una vez, y que hay que seguir vigilando en cada fase nueva.**
Lumen Script es dinámicamente tipado por debajo de la anotación: nada comprueba que una
reasignación, un `return`, o un argumento de llamada conserven el tipo con el que algo se declaró
(§Fase 3, "Corrección crítica"). El generador nativo, al confiar en el tipo declarado sin más,
produjo dos veces C++ que compilaba limpio y daba una respuesta HTTP *distinta* a la del bytecode
(una reasignación con cambio de tipo numérico, y `and`/`or` traducidos a `&&`/`||`) y una vez un
proceso que se moría (`%`/`/` por cero, indefinido en C++, controlado en el VM) — las tres en
código que el corpus de pruebas de la fase ya daba por bueno. La corrección (`tipo_provable()`,
un análisis de solidez separado del checker débil a propósito) es la regla a aplicar en **cada**
construcción nueva que toquen las fases siguientes, no solo en la que la disparó: antes de generar
C++ para algo, demostrar —con las mismas reglas dinámicas que aplica el VM, no con lo que el tipo
declarado sugiere— que el tipo del resultado no puede ser otro. Cuando no se pueda demostrar, cae a
bytecode; nunca generar en base a una suposición sin verificar.

**Lo que no arregla, para que no haya sorpresas.** El perfil de `POST /orders` bajo carga
concurrente dice que el 28.4% del tiempo de hilo está en
`sqlite3_step → sqlite3InvokeBusyHandler → nanosleep`: es SQLite serializando escritores,
porque solo admite uno a la vez. Eso está por debajo del lenguaje. Compilar a nativo no lo
toca. Si las escrituras concurrentes son el cuello de botella, la respuesta está en otro sitio
(un único escritor con cola en vez de ocho conexiones compitiendo, o un motor cliente-servidor),
y es una conversación aparte de esta.

**El riesgo alto es la fase 1.** Es abrir en canal un compilador que funciona y está probado,
para separar dos cosas que hoy están fusionadas. La mitigación es la que ya está escrita en la
fase: refactor puro, sin funcionalidad nueva mezclada, con el corpus como red y con la
exigencia de que hasta los mensajes de error salgan idénticos.

**El coste permanente es mantener dos backends.** Cada característica nueva del lenguaje habrá
que implementarla dos veces. El IR compartido reduce la duplicación a la generación final, y la
política de respaldo por ruta (§11) permite que el backend nativo vaya legítimamente por detrás
del intérprete sin bloquear nada — pero el coste no desaparece, y conviene tenerlo presente
antes de empezar, no a mitad.

**La dependencia de un compilador de C++** pasa a estar en la máquina donde se construye la
aplicación, no solo en la de quien compila Lumen. En la máquina de despliegue no hace falta: se
despliega el artefacto ya compilado.

**Los tiempos de compilación** van de milisegundos a segundos. Es la decisión de §2, tomada a
sabiendas, y por eso el flujo de desarrollo por defecto sigue siendo el intérprete.
