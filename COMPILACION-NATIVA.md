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

**Cuarto corte: `Dict<string, V>`, con la misma semántica de referencia — y un límite deliberado.**
`dict_runtime_prelude()` define `LDict<V>` con el mismo diseño que `LList<T>` (caja con refcount no
atómico, semántica de referencia real). La sintaxis genérica de Lumen es siempre `Dict<K, V>` aunque
`K` sea forzosamente `string` (§8 de la gramática) — `Type::from_declared` descarta `K` y solo
conserva `V`; `Dict<int>` con un único argumento se interpreta como *clave* `int` (descartada) y
*valor* `Json` por defecto, no como `Dict<string, int>` — un detalle de la gramática existente que
esta fase se limitó a descubrir, no a decidir.

El límite deliberado: **esta fase no admite leer un `Dict` por índice** (`d[k]`). El motivo es el
mismo que ya obligó a rechazar `a / b` entre dos `int` — `d[k]` con una clave *ausente* da `null` en
el VM (`vm.cpp::GetIndex`), un tipo distinto del valor declarado que esta fase no puede representar
en una ranura de tipo fijo. `Comprobador::tipo_provable()` ya rechazaba cualquier `Index` sobre algo
que no fuera `List` desde el corte anterior — aquí simplemente se documenta que es intencional para
`Dict`, no un olvido. Sí se admite **escribir** por índice (`d[k] = v`, siempre válido en el VM, sin
esa ambigüedad) y los dos métodos de `metodos_de()` para `Dict` que tienen sentido fuera de un
contexto de ruta: `has`/`keys` (el tercero, `save`, solo existe sobre un `File` subido).

Un literal `{k: v, ...}` reveló un límite real de C++, no un error propio: a diferencia de
`LList{1, 2, 3}` (CTAD deduce `T` directamente de cada elemento), `LDict{{k1,v1}, {k2,v2}}` intenta
deducir `V` *a través de* la construcción por lista de cada `std::pair<string,V>` — y la deducción de
argumentos de plantilla no mira dentro de una lista de inicialización anidada de esa forma (se
confirmó el rechazo directamente contra `g++`, con y sin guía de deducción explícita, antes de
descartar el enfoque). La solución: con `V` ya demostrado por `tipo_provable()`, generar
`LDict<V>{...}` con el argumento de plantilla explícito en vez de confiar en CTAD — lo que llevó a
que `Generador` dejara de llevar solo un mapa de ranura→tipo y pasara a guardar una referencia al
propio `Comprobador` ya usado con éxito, para poder volver a preguntarle el tipo de cualquier
expresión (el valor de un `DictLit`, el elemento de un `for`) sin duplicar el análisis.

Validado en `tests/native_build_shadow.cpp` (`prueba_diccionarios()`): un literal, escritura por
índice, `has()`/`keys()`, paso como parámetro a otra función nativa (`Dict` en la frontera se queda
sin *wrapper*, igual que `List`/`string`, pero compila igual — y `cuenta_claves()` recorre las claves
devueltas por `keys()` con un `for`, ejercitando la cadena completa `Dict → List<string> → for`), y
semántica de referencia (dos variables sobre el mismo diccionario). Coincide entre bytecode y
`--native` en todos los casos. 79/79 del corpus real y el canario `LUMEN_SHADOW_CHECK` siguen en
verde.

**Quinto corte, el último de la fase: clases de usuario.** Confirmado contra la investigación del
compilador real (no asumido): una clase en Lumen Script no tiene herencia, ni interfaces, ni
despacho dinámico, ni sobrecarga de métodos — solo sobrecarga de *constructores*, distinguidos por
aridad (`ClassSig::ctors`, un `map<aridad, índice>`). Los campos están restringidos por el propio
compilador a escalares (`project.cpp`), así que una instancia es, por construcción, un registro
plano — la única complicación real es que un campo puede ser `?` (opcional), y esta fase, como en
todo lo demás, deja `?` fuera.

`generar_clase_runtime()` genera, por clase, un `struct <Clase>_Box` (campos con prefijo `f_`, para
no chocar con el propio `rc`) y una clase envoltorio `L<Clase>` con semántica de referencia real
(§8) — mismo diseño que `LList`/`LDict`, sin plantilla porque cada clase tiene su propio conjunto
fijo de campos tipados, no un elemento homogéneo. Un único accesor por campo,
`campo_<nombre>()`, que devuelve una **referencia** — sirve para leer (`Member`) y para escribir
(`Assign` a `Member`: `p.campo_x() = v`) sin necesitar un par *getter*/*setter*. El único
constructor de `L<Clase>` (aparte de copia/movimiento) toma un valor por campo, en el orden de
declaración — y es, a la vez, la única forma de construir una instancia en C++ y el automapeo del
único constructor Lumen que esta fase compila (ver el límite de abajo): no hace falta ningún
símbolo `l_new_X` aparte, `ClassName(args...)` en el IR se traduce, literal, a `LClassName(args...)`.

Un método se genera como **función libre**, no como método C++ de `L<Clase>` — mismo patrón que ya
usa el resto del backend (prototipos antes que cuerpos, para que el orden de declaración en el
`.lum` no importe) y evita que la clase C++ necesite conocer, dentro de su propia definición, el
cuerpo (potencialmente arbitrario) de cada método. El receptor va como primer parámetro explícito
(`L<Clase> l_this`), igual que en el IR (`this` ocupa la ranura 0 en un método — al revés que en un
constructor, donde ocupa la última — ver `Emitter::check_method`/`check_ctor`). El símbolo se
nombra `l_<Clase>_<método>`, no solo `l_<método>`: Lumen no tiene sobrecarga, pero dos clases
*distintas* sí pueden compartir el nombre de un método, y cada clase es su propio espacio de
nombres.

**El límite deliberado, más estrecho que en los cortes anteriores: solo el constructor SIN
cuerpo.** El constructor con cuerpo tiene un problema real que ningún otro corte de esta fase tenía
todavía: `emit_ctor` arranca la instancia con **todos** los campos a `null` (`MakeDict`) y solo
sobrescribe los que el cuerpo toca explícitamente — verificar que un cuerpo arbitrario (con
`if`/`while`) deja **todos** los campos con su tipo declarado en **todas** las rutas posibles exige
un análisis de asignación definida que esta fase no hace. El constructor sin cuerpo (declarado así,
o el implícito que sintetiza `project.cpp` cuando la clase no declara ninguno) no tiene ese
problema: automapea un parámetro a cada campo, en el mismo orden, así que cubre todos los campos
por construcción — es exactamente la firma del único constructor C++ que genera
`generar_clase_runtime()`. `Comprobador::tipo_provable()` exige, para un `ConstructorCall`, que el
número de argumentos sea igual al de campos (rechaza un constructor sin cuerpo con MENOS parámetros
que campos, que dejaría alguno en `null`) y que cada uno demuestre exactamente el tipo del campo en
la MISMA posición (`RolFuncion::tiene_cuerpo` descarta cualquier constructor con cuerpo, sea cual
sea su aridad).

**El límite que importa de verdad: esta pieza es correcta, pero hoy es inalcanzable.** A diferencia
de `string`/`List`/`Dict` (verificados sirviendo HTTP de verdad), no hay manera de ejercitar una
clase nativa desde un programa Lumen en ejecución todavía, por dos motivos estructurales, no por
ningún descuido de esta fase:

1. **Una función suelta no puede tocar una clase.** Confirmado contra el compilador real (no
   asumido): `build_functions()` en `project.cpp` construye el `Emitter` de cada `fn` con
   `classes_ = nullptr` — el checker real *nunca* resuelve `ConstructorCall`/`ClassMethodCall`
   dentro de una función suelta, solo dentro de una ruta o un método (que sí reciben `ClassSigs`).
   `fn int f(): Punto(1, 2)` es, literalmente, un error de compilación hoy. `compilar_nativo()`
   respeta esto a propósito, con el mismo `classes_ = nullptr` en el `Emitter` de cada función
   suelta — pasarle `ClassSigs` ahí haría a esta fase *más permisiva* que el compilador real, la
   misma familia de divergencia que motivó la corrección crítica de más arriba.
2. **Un método nunca cruza la ABI.** El receptor es siempre de tipo clase, y una clase nunca es
   `tipo_abi_soportado()` — así que ningún método tiene nunca un *wrapper*, y no hay ninguna otra
   vía hoy (las rutas, que sí podrían llamar a un método, no se compilan a nativo hasta la Fase 4)
   para que el VM llegue a invocar código de clase nativo.

`compilar_nativo()` maneja esto con seguridad — un programa con clases pero sin ninguna función
suelta que cruce la ABI simplemente no tiene nada que ofrecer (`generadas.empty()` → `nullptr`, sin
aviso de error) — pero significa que, hasta que la Fase 4 compile rutas, el código que genera esta
pieza no tiene ningún punto de entrada real desde `--native`. Sigue siendo trabajo necesario, no
prematuro: es exactamente lo que la Fase 4 va a necesitar para que una ruta que construye y usa una
instancia se pueda compilar — y, mientras tanto, no hace daño: probado contra
`tests/casos/clases.lum` (el caso canónico del corpus real, con `Alta`/`Punto` y sus reglas
`validate:`), `--native` sigue compilando limpio las funciones sueltas del fichero (`doble`,
`factorial`, `infinita`) sin que el código de clase generado internamente cause ningún problema.

Validado, por tanto, de forma distinta al resto de esta fase: no contra el binario real sirviendo
HTTP, sino en `tests/native_class_shadow.cpp`, que genera el C++ de una clase y sus métodos
directamente (`generar_clase_runtime()`/`generar_metodo_nativo()`, sin pasar por
`compilar_nativo()`) y lo ejecuta con un `main()` propio — el mismo patrón que
`native_gen_shadow.cpp` usó para `fib`/`cuenta_primos` antes de que existiera `compilar_nativo()`.
Cubre: campos y un método (`this.x`, aritmética), un `ConstructorCall` **dentro** de un método que
devuelve una instancia nueva (`Punto(this.x + dx, this.y + dy)`), mutación de campo
(`this.x = this.x + dx`, `Assign` a `Member`), y el caso que de verdad importa según §8 — dos
variables sobre la misma instancia, mutar una a través de la otra, y comprobar que las dos ven el
cambio. Coincide con la VM en todos los casos. 79/79 del corpus real y el canario
`LUMEN_SHADOW_CHECK` siguen en verde.

### Fase 4 — Rutas HTTP síncronas
- Handler generado como `Task<void>` (o función síncrona si no hay `await`, §9), registrado en
  el router igual que hoy.
- Parámetros de ruta y query, binding del cuerpo a clase, `validate:`, `require`, `on error`,
  encadenado `status`/`header`/`cookie`.
- **Aceptación:** todos los endpoints del banco de pruebas que no tocan la base de datos
  devuelven respuestas idénticas en los dos modos, incluidos los casos de error (404 sin
  cuerpo, 422 con la lista de mensajes, 400 fuera de rango).

**Lo que la investigación destapó antes de escribir nada — cuatro decisiones que corrigen la
sección 9/11 de más arriba, no solo detalles de esta fase:**

1. **El ABI POD (`NativeValue`) no alcanza.** Una ruta necesita `lumen::Request&`/`lumen::Response&`
   de verdad — con `shared_ptr`, `std::function`, `unordered_map` por dentro — para leer parámetros
   y escribir la respuesta. No hay forma honesta de empaquetar eso en una unión de 8 bytes. Esta
   fase abandona, a propósito y solo para rutas, la pureza "el `.so` es autocontenido, ni cabeceras
   ni enlazado real" que Fase 2/3 mantuvieron — el `.cpp` generado para una ruta incluye
   `<lumen/request.hpp>`/`<lumen/response.hpp>` de verdad y el `.so` enlaza contra el runtime real
   (`liblumen`/`liblumen_script`). Es una decisión consciente, consultada antes de tomarla: el
   backend nativo deja de ser un artefacto que solo necesita el mismo compilador para su propio
   POD, y pasa a necesitar el mismo compilador **y la misma libstdc++** que el binario `lumen` —
   coste real de despliegue, aceptado porque sin él no hay sesión, ni JWT, ni *binding* de cuerpo,
   ni plantillas posibles nunca, en ninguna fase futura.
2. **No existe un camino síncrono en el router hoy.** El párrafo de §9 que dice que un handler sin
   `await` "se registra por el camino síncrono que `HandlerTraits` ya sabe manejar" describe una
   capacidad que hay que **construir**, no una que exista: `HandlerTraits::invoke` es ella misma una
   corrutina en las dos ramas (`handler_traits.hpp`), y `Router::add`/`add_internal` solo admiten
   `Task<void>`. La vía elegida —sin tocar el motor `lumen`— es la que ya usan las rutas
   **declarativas** (`project.cpp:1521-1525`): envolver el puntero de función nativo en una
   corrutina trivial, `[fn](Request& req, Response& res) -> Task<void> { fn(req, res); co_return; }`,
   en el propio `build_routes`. El ahorro real no es "sin frame de corrutina" (sigue habiendo uno,
   igual que en el camino declarativo) sino sin VM: nada de pila de operandos, sin recorrer bytecode.
3. **`on error` no es un `catch` alrededor del handler.** Al contrario de lo que sugiere §10, hoy se
   dispara **después** de que el handler termine, a nivel de motor (`App::handle_request`,
   `status_code() >= 400`) — y también para errores que ni siquiera llegan a ejecutar el cuerpo
   (404 del router, 422 de `bind_body`, 400 de `prepare_args`). Esto simplifica Fase 4 en vez de
   complicarla: un handler nativo no necesita ningún mecanismo de manejo de errores propio para que
   `on error` funcione — basta con que ponga el código de estado correcto vía `res.status(...)`, y
   el motor (sin cambios) hace el resto, exactamente igual que con un handler de bytecode.
4. **Una ruta produce el mismo `IrBlock` que una función.** `check_route` antepone las guardas de
   grupo al cuerpo como `IrStmtKind::Require` normales (`emitter.cpp:186-202`, compartiendo
   `check_require_like` con el `require` del cuerpo) — no hay ningún nodo de IR específico de rutas
   que tratar aparte. Consecuencia práctica: dar soporte a `IrStmtKind::Require` beneficia a la vez
   a las guardas de una ruta y a un `require` dentro de una función suelta.

**Primer paso: `require`.** `Comprobador::tipo_provable`/`stmt_compilable` ganan el caso que
faltaba: la condición solo necesita ser demostrable en algún tipo (igual que `If`/`While`), y
`otherwise` tiene que demostrar EXACTAMENTE el tipo de retorno de la función — la misma regla que ya
exige un `Return`, porque `require cond else otherwise` es, literalmente, "si no `cond`, `return
otherwise`" (`emitter.cpp:1750-1759`, el mismo patrón que genera `Generador::stmt`: `if (!(cond)) {
return otherwise; }`). Validado en `tests/native_build_shadow.cpp` (`prueba_require()`) y contra el
binario real sirviendo HTTP: `require n >= 1 and n <= 10 else -1` da la misma respuesta en las dos
vías, dentro y fuera de rango. 79/79 del corpus real y el canario `LUMEN_SHADOW_CHECK` siguen en
verde.

**Segundo hallazgo, otra corrección crítica — un cuerpo que "cae al final" es UB en C++, no `null`.**
Escribir la prueba de `require` hizo probar, a propósito, el pariente más simple: una función sin
`else`, `fn int f(int n): if n > 5: return n`, sin nada después del `if`. El compilador real la
acepta sin ningún aviso — si el cuerpo se acaba sin `return`, el VM devuelve `null` con toda
naturalidad, es exactamente el mismo caso que ya cubre el `Require` de arriba. Pero antes de esta
corrección, `--native` generaba una función C++ no-`void` que podía llegar al final de su cuerpo sin
ningún `return` — **comportamiento indefinido** en C++, no "el mismo `null`" — y, confirmado contra
el binario real, el resultado no era ni siquiera un valor plausible: `bytecode` daba `{"r":null}`
para `f(3)`, `--native` daba `{"r":3}` — basura de la pila de C++ que por pura coincidencia parecía
un valor razonable. Es la misma familia de fallo que la corrección crítica original (§ más arriba):
un tipo declarado que el compilador real no garantiza en ningún sitio, y esta fase, en su primera
versión, sí asumía.

La corrección: `bloque_siempre_retorna()` recorre el cuerpo demostrando si SIEMPRE termina en un
`return` — un `if`/`else` cuenta solo si las DOS ramas lo garantizan (igual que exige el propio
compilador de C++ para el mismo patrón); un `require` nunca cuenta por sí solo (solo cubre el camino
"condición falsa"); un `while`/`for` tampoco (el cuerpo puede ejecutarse cero veces). Para cualquier
función con retorno no-`void`, no poder demostrarlo la descarta entera — no se sintetiza ningún
`return` con un valor por defecto (sería inventar un `0`/`""` que el VM no tiene: la respuesta real
es `null`, un tipo que esta fase no puede representar de todas formas). Validado en
`tests/native_build_shadow.cpp` (`prueba_sin_return_en_todos_los_caminos()`) y de nuevo contra el
binario real: la función ahora se queda en bytecode (`0 funcion(es) compilada(s)`) y las dos vías
coinciden. `fib`/`cuenta_primos`, los métodos de clase, y el resto del corpus real (`bench/lumen/
app.lum`, `tests/casos/clases.lum`) se re-verificaron sin cambios — todos ya retornaban en todos sus
caminos, la corrección es más estricta, no distinta, para el código que ya era seguro.

**El valor de retorno de una ruta necesita una representación distinta a la de una función — resuelto.**
`Dict<K,V>` (Fase 3) exige un valor `V` homogéneo — pero el cuerpo JSON de una respuesta real casi
nunca lo es (`{"n": n, "result": r}` ya mezcla dos expresiones que, en general, no comparten tipo; el
caso de `bench/lumen/app.lum` con `int`/`string`/`double`/`bool` en un dict es aún más heterogéneo).
La solución: `Comprobador::es_valor_json()`, un predicado hermano de `tipo_provable()` pero más
permisivo — acepta escalares y `DictLit`/`ListLit` anidados de eso mismo, SIN exigir que compartan
tipo (a diferencia de `tipo_provable`, que sí lo exige para un contenedor nativo) — y
`Generador::valor_json()`, que construye el equivalente en `lumen_script::Value` (el tipo dinámico
del VM, con `to_json_text()`) en vez de un contenedor nativo homogéneo: cada entrada se convierte por
su cuenta, recursivamente, con `Value::integer/real/boolean/str` para los escalares y una lambda
autoinvocada (`Value::Dict`/`Value::List` construidos a mano) para los literales. El orden de las
claves coincide con el `.lum` porque `Value::Dict` es un vector en orden de inserción, no un `map`
(`value.hpp:35-42`), y `valor_json()` recorre `entries` en ese mismo orden. Queda fuera, a propósito,
convertir una `List<T>` YA construida (un `Ident`, por ejemplo) a `Value`: exigiría iterar `LList<T>`
en el `.cpp` generado, y ningún caso de prueba lo necesita todavía — `Dict<V>` como valor de
respuesta también queda fuera (`LDict` no expone iterar sus pares).

**El enlazado real — la otra pieza que Fase 2/3 no necesitaban.** `Value::write_json()` (que
`to_json_text()` llama) no es inline: vive en `value.cpp`, dentro de `liblumen_script.a`. Un `.cpp`
de ruta que la usa tiene que **enlazar** contra esa biblioteca, no solo incluir su cabecera — la
primera vez que esta fase depende de código real del proyecto, no de texto autocontenido. Dos
cambios en `CMakeLists.txt` lo hacen posible: `CMAKE_POSITION_INDEPENDENT_CODE ON` global (un `.a`
compilado sin `-fPIC` no se puede meter dentro de un `.so`, y es más simple que marcarlo target a
target incluyendo las dependencias transitivas) y dos macros horneadas en tiempo de compilación de
CMake (`LUMEN_NATIVE_INCLUDE_DIR`, `LUMEN_NATIVE_SCRIPT_LIB`) que le dicen a `native_build.cpp`, en
tiempo de ejecución del binario `lumen`, dónde está `include/` y dónde quedó `liblumen_script.a` ya
compilada — no hay otra forma de que un binario ya enlazado lo adivine. `lumen::Response` y
`lumen::Request` resultaron ser enteramente *header-only* (todos sus métodos son inline en
`response.hpp`/`request.hpp`), así que esta primera ruta NO necesita enlazar `liblumen.a` — solo
`liblumen_script.a`, y solo cuando el módulo tiene al menos una ruta nativa (`compilar_nativo` arma
un `.cpp` distinto, con cabeceras y enlazado real, únicamente en ese caso — el `.so` autocontenido de
Fase 2/3 para un módulo sin rutas compilables no cambia ni un carácter).

**`generar_ruta_nativa()` — más simple que una función porque nadie más la llama.** Una ruta nunca
cruza la ABI POD de `native_abi.hpp`: nadie la invoca desde bytecode, solo `build_routes()` en C++
normal, así que su símbolo `extern "C"` puede tener la firma real que hace falta
(`lumen::Request&, lumen::Response&`) en vez de la genérica `NativeValue*`. Eso, a su vez, elimina la
necesidad de `bloque_siempre_retorna()` para rutas: la función generada es `void`, así que "caer al
final" es C++ perfectamente válido (nunca UB) — y es, además, EXACTAMENTE lo mismo que hace el VM
cuando el cuerpo de una ruta termina sin `return` explícito (`null` → 204). `generar_ruta_nativa()`
simplemente antepone `res.status(204).send("");` al final de cada ruta generada: inalcanzable si ya
hay un `return`/`require` en todo camino, y el 204 correcto si no lo hay.

Alcance deliberadamente estrecho de este primer corte: solo parámetros escalares de patrón (`:id`) o
query string — un `File`, un parámetro de tipo clase (cuerpo de petición), o cualquier `?` (opcional)
dejan la ruta entera sin compilar, igual que un cuerpo que use `session`/`jwt`/`render`/`await`
(`Comprobador::block_compilable` ya los rechaza sin necesitar ningún caso nuevo: no forman parte de
ningún `IrExprKind`/`IrStmtKind` que reconozca — el mismo invariante de "no hay generación parcial"
que ya regía funciones). El *binding* de parámetros se reproduce a mano dentro de la ruta generada,
EXACTAMENTE con las mismas reglas que `prepare_args()`/`coerce()` en `project.cpp`: ausente → cero
silencioso del tipo (o el valor por defecto, si el parámetro es de query y lo declara — un `int
limit = 20` extrae el texto del literal en el propio AST, en tiempo de compilación, y lo hace pasar
por el MISMO `coerce()` que un valor real, así que un defecto mal tipado a propósito daría el mismo
400 que uno real; un parámetro de PATRÓN con defecto es, como en `bind_params`, un error de
compilación, nunca algo que este generador intente resolver); presente pero sin parsear → el mismo
400 `{"error":"parametro invalido","param":...,"esperado":...,"recibido":...}`, incluido el detalle
de que `std::stoll`/`std::stod` aceptan basura al final sin comprobarlo, `"12abc"` → `12`. No hay
manera de llamar a `prepare_args` desde el `.cpp` generado (vive en otro binario, sobre `Value`, no
sobre los tipos C++ que declara la ruta), así que se duplica a propósito, con `route_runtime_prelude()`
antepuesto una sola vez por módulo. Cuando la ruta entera es representable, el handler nativo
sustituye ENTERAMENTE a `bind_params`/`prepare_args`/`begin_auth`/el VM en `build_routes` — no los
llama, es código generado que hace su propio trabajo.

**Validado en tres capas.** `tests/native_route_shadow.cpp` compila el mismo fuente con
`compile(..., native=false)` y `compile(..., native=true)` y despacha peticiones directamente contra
`mod->router` (sin sockets: el `Task<void>` de una ruta sin `await` nunca suspende de verdad, un
`handle.resume()` basta) para el patrón bandera de `bench/lumen/app.lum`:

```lum
get endpoint("/compute/fib/:n", int n):
    require n >= 1 and n <= 32 else status(400)
    int r = fib(n)
    return { "n": n, "result": r }
```

comparando status+cuerpo byte a byte en tres casos (`n=10` normal, `n=50` rechazado por la guarda,
`n="abc"` que no parsea) y, sobre una segunda ruta con un parámetro de query CON defecto
(`get endpoint("/compute/fibq", int n = 5)`), tres más (ausente → usa el defecto, presente lo
sustituye, mal tipada → el mismo 400) — las seis coinciden. Contra el binario real, sirviendo HTTP de verdad, con
`bench/lumen/app.lum` completo en dos instancias (una `--native`, otra sin): `/compute/fib/10`,
`/compute/fib/32` (límite), `/compute/fib/33` y `/compute/fib/0` (rechazados por la guarda),
`/compute/fib/abc` (parámetro inválido) y `/compute/primes/1000` dan la respuesta IDÉNTICA en las dos
vías, incluido el `{"error":"peticion no valida"}` que sirve el `on error 400:` de la aplicación —
confirmación en vivo de que "`on error` es responsabilidad del motor, no del handler" (hallazgo 3 de
más arriba) es exactamente lo que hace que un handler nativo no necesite ningún mecanismo de error
propio. `bench/lumen/app.lum` compiló 2 funciones (`fib`, `cuenta_primos`) y 3 rutas a nativo
(`/health`, `/compute/fib/:n`, `/compute/primes/:n`); `/slow/:ms` (usa `await`) y `/payload/:n` (usa
`List<Json>`) se quedan en bytecode, como deben. Curiosidad sin impacto observable: `/health` compila
a nativo (su cuerpo — un único `return` de un dict de un literal — cae dentro del alcance de esta
fase) pero nunca se usa por esa vía, porque `try_declarative` (Nivel 1, cero bytecode) la captura
primero en `build_routes` — `compilar_nativo()` no sabe nada de esa prioridad, así que compila la
ruta igual; el símbolo queda resuelto en `NativeModule::rutas_por_indice` sin que nadie lo llame.
Trabajo desperdiciado pero inofensivo, no una divergencia. 79/79 del corpus real (sin `--native`,
donde nada de esto se ejerce) y las 8 suites de `ctest`, `native_route_shadow` incluida, en verde.

**Efecto colateral: las clases dejan de ser universalmente inalcanzables.** Fase 3 documentó que
ninguna clase era alcanzable desde un programa Lumen en ejecución (una función suelta no puede
tocarlas; un método nunca cruza la ABI). Eso seguía siendo cierto para el *binding* de parámetros de
esta fase (un parámetro de tipo clase, cuerpo de petición, queda fuera a propósito) — pero
`generar_ruta_nativa()` reutiliza el mismo `Comprobador`/`Generador` que ya sabían compilar
`ConstructorCall`/`ClassMethodCall` cuando se les pasa `TablaClases`/`TablaRoles`, y `check_route`
(igual que `check_method`) sí recibe `ClassSigs`. Una ruta que construye y usa una instancia
ENTERAMENTE dentro de su cuerpo (sin que cruce nunca un parámetro o el valor de retorno) sería, en
teoría, la primera vía real de ejecución para código de clase nativo — sin explorar ni probar
todavía en este corte, y `es_valor_json()` tampoco sabe serializar una instancia como valor de
retorno, así que el caso útil (una clase en la respuesta) sigue sin cubrir.

**Siguiente incremento: `Comprobador::es_llamada_respuesta()` generaliza `es_llamada_status()` a las seis
funciones globales de `natives.cpp` que escriben la respuesta ELLAS MISMAS y devuelven `null`
(`status`, `text`, `html`, `json`, `redirect`, `send_file`) — utilizables tanto como el `otherwise` de
una guarda (`require ... else redirect("/login")`) como el propio valor de un `return`
(`return status(204)`), cosa que antes de este incremento ni siquiera compilaba: `tipo_provable()` no
sabe nada de `BuiltinGlobalCall` en general, así que `return status(204)` caía siempre a bytecode
completo, no solo el `require`. `Generador::codigo_llamada_respuesta()` genera la llamada directa
sobre `res` — para `text`/`html`, pasando el argumento (solo escalares, no una estructura: son texto
o número, no JSON) por `valor_json()` y llamando a `.to_string()` sobre el `Value` resultante, la
MISMA función que usa `fn_text`/`fn_html`, así que el formato de un `float`/`bool` convertido a texto
coincide por construcción, sin reimplementarlo. `json(v)` reusa `es_valor_json()` entero (cualquier
cosa que ya sabe serializarse como respuesta puede pasarse a `json()` explícitamente).
`tests/native_route_shadow.cpp` gana tres rutas más y cinco casos (guarda con `text()`, `return
html()`, `return status(204)`, guarda con `redirect(url, código)`, `return redirect(url)`) — el
`Resultado` de la prueba ahora compara también la cabecera `Location`, no solo status+cuerpo. 79/79
del corpus y las 8 suites de `ctest` en verde.

**`str()`/`len()`: los dos primeros `BuiltinGlobalCall` puros que demuestra `tipo_provable()`.** No es
una pieza de rutas — beneficia a cualquier función/método nativo, aunque nació al querer escribir
`text("resultado: " + str(n))` en una ruta. `str(x)` solo acepta un escalar (reusa el mismo puente a
`Value` que el valor de retorno de una ruta, `Generador::valor_json()`, y llama a `Value::to_string()`
— la MISMA función que `fn_str`, así que el formato de un `float`/`bool` convertido a texto coincide
por construcción). `len(x)` solo `string`/`List<T>` — nunca `Dict` (`LDict` no expone ningún método de
tamaño, ver `dict_runtime_prelude`) — y traduce a lo que cada tipo expone de verdad: `.size()` para
`std::string`, pero `.lumen_len()` para `LList<T>` (no tiene `.size()`), una elección real según el
tipo demostrado, no una única llamada genérica que hubiera compilado por casualidad para uno de los
dos y fallado en silencio para el otro.

**Bug real encontrado escribiendo la prueba, corregido antes de commitear — `Value` no siempre estaba
declarado.** `str()` puede aparecer en CUALQUIER función/método nativo, no solo en una ruta — pero el
`#include <lumen_script/value.hpp>` + `using lumen_script::Value;` que necesita se anteponía
"solo si el módulo tiene alguna ruta" (`con_rutas`, ver el incremento del enlazado real, más arriba):
un módulo con una función que usa `str()` y NINGUNA ruta fallaba a compilar con `'Value' has not been
declared`, confirmado escribiendo `tests/native_build_shadow.cpp::prueba_str_len()` (funciones
sueltas, sin ninguna ruta en el programa de prueba) antes de arreglarlo. Es la misma familia de
descuido que las dos correcciones críticas anteriores: una condición de guarda que servía para el
caso que la motivó (rutas) dejaba de servir en cuanto algo la reutilizó desde otro contexto (una
función suelta) sin que nadie volviera a mirarla. La corrección: el `include`/`using` de `Value` (y el
enlazado contra `liblumen_script.a`, y el `-I` que lo hace posible) ahora son incondicionales siempre
que se vaya a compilar algo — solo `<lumen/request.hpp>`/`<lumen/response.hpp>`/
`route_runtime_prelude()` siguen exclusivos de `con_rutas`, porque esos sí son privativos de una ruta.
`prueba_str_len()` queda como regresión permanente en `tests/native_build_shadow.cpp`. 79/79 del
corpus y las 8 suites de `ctest`, incluida `native_route_shadow`, en verde tras la corrección.

**Tercera corrección crítica — una ruta nativa no tenía ningún wrapper que atrapara el canal de
error.** El mismo canal que usan `lumen_div_check`/`lumen_mod_check`/el índice de `LList`
(`lumen_native_fail()` → lanza `LumenNativeError`, ver `error_runtime_prelude()`) solo lo atrapa,
para una función, el wrapper `extern "C"` que genera `generar_funcion_nativa()` — pero una ruta NO
tiene ningún wrapper: nadie la invoca a través de la ABI, la llama `build_routes()` directamente en
C++ normal. Antes de esta corrección, `generar_ruta_nativa()` no envolvía el cuerpo en ningún
try/catch, así que un `a % b` con `b` dinámico (ya demostrado seguro por `Comprobador`, pero no
exento de fallar en tiempo de ejecución) escapaba de la corrutina sin que nadie la atrapara. Probado
a propósito contra el binario real (no una precaución especulativa): **no tumbaba el proceso**
(`Task<void>::promise_type::unhandled_exception()` la absorbe con un `catch (...)`), pero daba
`{"error":"Internal Server Error"}` — el genérico de `http_connection.cpp` para una excepción sin
atrapar — en vez de `{"error":"modulo por cero","en":"..."}` que da bytecode: 500 en las dos vías,
pero un cuerpo distinto — la misma clase de divergencia silenciosa que las dos correcciones críticas
anteriores, encontrada por la misma disciplina de probar el caso incómodo a propósito en vez de
asumir que "ya demostrado seguro para generar" implica "seguro en tiempo de ejecución sin red de
seguridad".

La corrección: `generar_ruta_nativa()` envuelve TODO el cuerpo (binding de parámetros incluido) en un
`try { ... } catch (const LumenNativeError&) { ... }` que arma la MISMA respuesta que
`build_routes()` construye para un error de bytecode — `{"error": <mensaje>, "en": <ubicación>}` con
500 — usando `lumen_native_error_message()` (el mismo mensaje, byte a byte) y `"MÉTODO patrón"` para
`"en"` en vez de un `archivo:línea:columna` preciso: el código nativo no lleva ninguna noción de
línea/columna en tiempo de ejecución, así que no puede reproducir ESE detalle exacto — es la única
divergencia deliberada y documentada de todo lo que cubre esta fase, confinada a un campo de
diagnóstico, nunca al mensaje de error en sí ni al código de estado. `tests/native_route_shadow.cpp`
gana una ruta (`/mod/:a/:b`, `int r = a % b`) y una comparación dedicada que exige el mismo status y
el mismo `"error"` ignorando `"en"`, más una petición posterior que confirma que la ruta nativa sigue
sirviendo después del error. 79/79 del corpus y las 8 suites de `ctest` en verde.

**`int(x)`: el tercer `BuiltinGlobalCall` puro, y el primero de los tres (`str`/`len`/`int`) que puede
fallar de verdad en tiempo de ejecución.** Identidad sobre `Int`, truncar hacia cero sobre `Float`/
`Bool` (el mismo cast que ya hace `fn_int` sobre `Value::as_float()`/`as_bool()`) y, sobre `String`,
un fallo real — `lumen_str_to_int()` (nueva, en `error_runtime_prelude()`) usa el MISMO
`std::stoll` sin comprobar cuánto consumió (`"12abc"` → `12`, igual que `fn_int`) y el mismo mensaje
EXACTO (`"int(): '<texto>' no es un número"`) por el mismo canal (`lumen_native_fail`) que división/
módulo por cero — precisamente el canal que la corrección crítica anterior ya dejó seguro tanto en
una función (su wrapper) como en una ruta (su try/catch), así que añadir el primer builtin puro que
falla de verdad no reabre ningún hueco: ya había red de seguridad esperándolo.
`tests/native_build_shadow.cpp::prueba_conversion_int()` prueba los cuatro escalares como argumento
(incluida la cadena inválida, comparando el mismo `Status::Error` con el mismo mensaje en las dos
vías) como regresión permanente. 79/79 del corpus y las 8 suites de `ctest` en verde.

**Cierre del "pendiente" original: `List<T>` YA construida como valor de retorno de una ruta.**
`Generador::valor_json()` tenía un hueco a propósito desde el primer incremento del puente a `Value`:
una `List<T>` que ya existe como variable (no un `ListLit` literal) no se convertía — habría exigido
iterar `LList<T>` en el `.cpp` generado, y ningún caso de prueba lo necesitaba todavía. Cerrado ahora
con `lumen_valor_de()` (`route_runtime_prelude()`): cuatro sobrecargas escalares más una plantilla
sobre `LList<T>` que recorre con `lumen_len()`/`lumen_get()` y llama a `lumen_valor_de()` sobre cada
elemento — nunca necesita más de un nivel de recursión real porque
`tipo_elemento_contenedor_soportado` ya prohíbe `List<List<..>>`. `Comprobador::es_valor_json()`
ahora acepta `Type::Kind::List` además de los escalares. `tests/native_route_shadow.cpp` gana una
ruta (`/rango/:n`, construye una `List<int>` con un bucle y la devuelve dentro de un dict junto a un
escalar) como regresión permanente. 79/79 del corpus y las 8 suites de `ctest` en verde.

**Lo mismo, cerrado también para `Dict<V>`.** `LDict` no exponía NINGÚN acceso indexado a sus pares
(a propósito: leer por clave es ambiguo, ver el comentario de `tipo_soportado`) — pero recorrer
TODOS los pares no tiene esa ambigüedad, así que `dict_runtime_prelude()` gana tres métodos nuevos
sin ningún equivalente en el lenguaje Lumen (`lumen_len()`, `lumen_key_at(i)`, `lumen_val_at(i)`,
solo para este puente) y `lumen_valor_de()` una sobrecarga sobre `LDict<V>` que los usa para construir
un `Value::Dict` en el MISMO orden de inserción (`LDict`, igual que `Value::Dict`, es un vector, no
un `map`). `Comprobador::es_valor_json()`/`Generador::valor_json()` tratan `List`/`Dict` igual a
partir de aquí. `tests/native_route_shadow.cpp` gana una ruta (`/contadores/:n`, construye un
`Dict<string,int>` con un bucle, con una clave calculada vía `"c" + str(i)` — ejercitando de paso la
concatenación con `str()` recién añadido) como regresión permanente. 79/79 del corpus y las 8 suites
de `ctest` en verde.

### Fase 5 — Asincronía y base de datos
- `await` → `co_await` sobre los awaitables existentes; transacciones, pool, `last_id`.
- **Aceptación:** el banco de pruebas completo (`bench/run_all.sh`) corre en modo `--native`
  con las mismas respuestas y sin errores de aplicación.

**Primer paso: `await sleep(ms)`.** El único `await` que esta fase representa por ahora — una
consulta a base de datos necesita mucha más plumbing (pool, *worker pinning*, transacciones) y
queda para el siguiente incremento. Decisión de diseño consciente: `await` solo se genera dentro de
una **ruta**, nunca en una función/método suelto — `Comprobador::usa_await()` (puesto a verdad
dentro de `tipo_provable()`, caso `IrExprKind::Await`) hace que `generar_funcion_nativa()`/
`generar_metodo_nativo()` **rechacen** cualquier cuerpo que lo use, porque una función suelta se
invoca directamente en C++ desde otra función nativa — nunca con `co_await` — y suspenderse a mitad
no tendría a quién avisar. Una ruta sí tiene quién: `build_routes()` ya sabe `co_await`-ar un
`Handler`. Cuando `usa_await()` da verdad, `generar_ruta_nativa()` genera la ruta entera como
`lumen::Task<void>` en vez de `void` (`RutaNativa::asincrona`) — una corrutina real — y
`Generador::ret_vacio()` centraliza la única diferencia mecánica: toda salida temprana pasa a ser
`co_return;` en vez de `return;` (una corrutina no admite un `return` a secas). `await sleep(ms)` se
traduce a `co_await lumen::sleep(lumen_clamp_sleep_ms(ms))` — el mismo `lumen::sleep()` y el mismo
piso de 1ms (`clamp_sleep_ms`) que ya usa una ruta de bytecode, así que el comportamiento —
suspensión real sobre el *event loop*, no un bloqueo de hilo — es idéntico, no solo el resultado
final. `NativeModule` gana una segunda tabla, `rutas_async_por_indice` (`Task<void>(*)(Request&,
Response&)`, distinta de la de rutas síncronas), porque las dos firmas no caben en un solo tipo de
puntero de función; `build_routes()` la registra **directamente**, sin ningún envoltorio — su firma
ya coincide exactamente con `Handler`.

**Dos fallos reales encontrados probando esto contra el binario de verdad, ninguno anticipado.**

1. *Un `dlerror()` de más — UB, no solo un mensaje feo.* `dlopen()` puede fallar de verdad (y de
   hecho falló, la primera vez que se compiló una ruta con `Task<void>`, ver el punto 2). El código
   de error construía el aviso con `dlerror() ? dlerror() : "..."` — dos llamadas. `dlerror()` tiene
   semántica de un solo uso: la primera devuelve el mensaje y lo consume: la segunda, en la misma
   expresión, ya da `nullptr`. `std::string + nullptr` no es "un mensaje raro", es comportamiento
   indefinido — aquí, un SEGV real, con pila de llamadas confirmándolo (`std::string::append` sobre
   un `char*` nulo). Corregido guardando el resultado en una variable y llamando a `dlerror()` una
   sola vez.
2. *El `.so` no compartía el `event_loop` con el binario — `await sleep()` no esperaba nada.* Con el
   fallo de arriba corregido, el mensaje real de `dlopen()` apareció: `undefined symbol:
   EpollLoop::post`. Enlazar `liblumen.a` además de `liblumen_script.a` (necesaria desde que una
   ruta usa `lumen::Task`/`lumen::Response` — antes solo hacía falta `liblumen_script.a`, para
   `Value`) lo arregló, pero destapó un fallo más sutil y más grave, que **no daba ningún error de
   compilación ni de carga**: `await sleep(200)` en una ruta nativa respondía en ~3ms, no ~200ms.
   Causa: `lumen::detail::current_loop`/`current_token` (`task.hpp`) son variables `inline
   thread_local` — con vinculación débil pensada para deduplicarse entre unidades de traducción,
   pero eso solo funciona de verdad entre módulos dinámicos si el símbolo del EJECUTABLE está
   exportado a la tabla dinámica; sin eso, el `.so` se lleva su PROPIA copia privada, siempre a
   `nullptr` porque nadie la toca ahí. `SleepAwaitable::await_suspend()`, con `loop == nullptr`,
   reanuda al acto (`if (!loop) { h.resume(); return; }`) — código defensivo pensado para un uso sin
   *event loop*, no para este caso, pero que enmascaraba el problema en vez de fallar ruidosamente.
   Corregido con `-rdynamic` en el ejecutable `lumen` (`target_link_options`): exporta sus símbolos
   para que el `.so` resuelva contra la copia del binario, no contra una propia. Verificado varias
   veces contra el binario real, con las dos vías compitiendo lado a lado en `bench/lumen/app.lum`
   (`/slow/:ms`): antes de la corrección, bytecode tardaba ~204ms y `--native` ~3ms para el mismo
   `sleep(200)`; después, los dos tardan ~204ms — y una cadena de tres peticiones secuenciales de
   300ms cada una, más una cuarta concurrente, confirma que la suspensión es real (no un
   bloqueo disimulado) y que el servidor sigue respondiendo con normalidad durante la espera.

`tests/native_route_shadow.cpp` gana una ruta (`/espera/:ms`) y dos casos (guarda que rechaza,
`await sleep()` normal) — funcionales, no de tiempo: sin un *event loop* real detrás, la prueba
aislada no puede medir milisegundos (ni falta que hace: la verificación de tiempo real es la de
arriba, contra el binario real). `bench/lumen/app.lum` pasa de 3 a 4 rutas nativas
(`/slow/:ms` se suma a `/health`, `/compute/fib/:n`, `/compute/primes/:n`). 79/79 del corpus y las 8
suites de `ctest` en verde.

**`await sqlite.query(...)`/`exec(...)`/`last_id()` — el slot `Value` dinámico, construido como su
propio incremento deliberado.** La primera versión de este documento dejó esto fuera, con el
argumento de que `run_db()` devuelve un `Value` cuyo tipo real depende de si el driver tuvo éxito
(`List<Json>` o, si falla, un `Dict` con `{"error": msg}`, **sin pasar por el canal de error de la
VM**) — exactamente lo que `tipo_provable()` está diseñado para rechazar (un tipo que no es SIEMPRE
el mismo). Ese análisis seguía siendo correcto; lo que cambió fue la conclusión: en vez de dejarlo
fuera, se construyó la única representación honesta que puede sostenerlo — un `Type::Kind::Json`
que ya existía como centinela puntual (el valor de retorno de una ruta) pasa a ser un tipo nativo de
pleno derecho, respaldado por `lumen_script::Value` de verdad, con sus propias reglas de
indexado/aritmética/comparación resueltas en tiempo de ejecución — exactamente del tamaño que se
había estimado (el de la Fase 2), construido con la misma disciplina (un operador a la vez,
verificado contra `vm.cpp` línea a línea, probado contra HTTP real con datos reales).

*El tipo.* `tipo_cpp(Json)` es `Value` — no una plantilla nueva. Un `List<Json>`/`Dict<string,Json>`
(la forma declarada de una fila o una tabla) se representa IGUAL, no como `LList<Value>`/
`LDict<Value>`: esas dos plantillas existen para contenedores HOMOGÉNEOS de tipo fijo (§8), y aquí
el propio `Value` ya sabe ser una lista o un diccionario por su cuenta — envolverlo en otra caja no
añadiría nada. `es_json_dinamico(Type)` trata las tres formas (`Json` suelto, `List<Json>`,
`Dict<string,Json>`) como una sola cosa en todo el generador, para no triplicar cada caso.

*De dónde sale un Json.* Solo de `await <módulo>.query/exec/last_id(...)` — nunca de una función
suelta ni de un parámetro declarado `Json` a mano (aunque, una vez que existe, SÍ puede pasarse como
argumento a otra función nativa: `tipo_soportado`/`tipo_provable` no le ponen ninguna barrera
especial ahí, `Type::operator==` ya trata dos `Json` como el mismo tipo). `begin()`/`commit()`/
`rollback()`, en esta versión del documento, ya tienen representación — ver Fase 5.6 más abajo.

*`VarDecl` deja de fiarse del tipo declarado cuando el valor real es Json.* `int stock =
filas[0]["stock"]` es Lumen válido — el tipo declarado es decorativo, Lumen Script nunca lo exige en
la asignación, solo en el uso (§ el resto de este documento ya lo explica largo y tendido) — así que
`Comprobador::stmt_compilable` registra el tipo REAL de la ranura (`Json`, no `Int`) cuando el valor
inicial es Json, sea cual sea la anotación; toda reasignación futura de esa ranura tiene que
demostrar Json también (la misma regla de inducción de siempre, sin excepción nueva). El `Value
stock = ...;` que sale de `Generador::stmt` en vez de un `int64_t stock = ...;` es la consecuencia
directa, no un caso aparte.

*Indexado, aritmética y comparación se resuelven en tiempo de ejecución, con la MISMA lógica que
`vm.cpp`, no una versión más permisiva ni más estricta.* `lumen_json_index_int`/`_str` reproducen
`Op::GetIndex` (una `List` con clave string, o una `Dict` con clave int, fallan con el MISMO mensaje;
una clave de `Dict` ausente da `null`, nunca un error). `lumen_json_add`/`_arit`/`_compare` reproducen
`Op::Add`/`Sub`/`Mul`/`Div`/`Mod`/`Lt`/`Le`/`Gt`/`Ge` (`numeric_pair()`/`compare()` de `vm.cpp`,
mensajes de error incluidos). `lumen_json_eq`/`_ne` son `Value::equals()` directo. Todos usan
`lumen_native_fail()` — el MISMO canal que división/módulo por cero — así que el try/catch que ya
envuelve cualquier ruta (la corrección crítica de más arriba) los atrapa sin necesitar nada nuevo:
añadir el primer conjunto de operaciones Json que puede fallar de verdad no reabrió ningún hueco,
otra vez. `len()`/`str()`/`int()` se extendieron con el mismo criterio (`lumen_json_len`, mismas
reglas que `fn_len`; `str()` reusa `Generador::valor_json()`, que ahora trata Json como identidad;
`int()` con `lumen_json_as_int`, mismas reglas que `fn_int`).

*Enlazar bytecode y `--native` al MISMO código, no a una reproducción aparte.* `run_db()`
(`project.cpp`) tenía toda la lógica real de una suspensión de base de datos atada a `VM::Result`/
`NativeCtx`. Extraída a `lumen_script::await_db()` (`db.hpp`/`db.cpp`, un `DbOp` en vez del
`native_id` de turno, los mapas `pinned_workers`/`last_exec_workers` por referencia en vez de un
`NativeCtx` entero) — `run_db()` pasa a ser un adaptador delgado sobre esto, y el código que genera
una ruta nativa llama exactamente a la misma función. Una ruta asíncrona declara sus propios
`l_pinned_workers`/`l_last_exec_workers` locales (equivalentes a los de `NativeCtx`, vivos durante
toda la petición) — necesarios de verdad solo para encadenar `exec()` con `last_id()` sobre la MISMA
conexión (`last_insert_rowid()` es específico de la conexión que hizo el `INSERT`), pero declarados
siempre que la ruta es asíncrona, se usen o no: más simple que detectar de antemano si el cuerpo
toca una base de datos.

*Un ICE real de GCC 13.3, no un error propio, encontrado al compilar la primera ruta con `await
sqlite.query(...)` de verdad.* `g++` fallaba con "internal compiler error: in
build_special_member_call" al ver un `std::vector<Value>{...}` (con al menos un elemento)
construido DIRECTAMENTE como argumento de una llamada que se hace `co_await` — aislado con un
reproductor mínimo hasta confirmar que ni el vector, ni los locales previos al `await`, ni el
try/catch, eran la causa por separado: la combinación exacta "braced-init-list de `Value` como
argumento inline de una llamada co-awaited" sí, de forma reproducible. Una llamada de función
NORMAL que construye y devuelve el mismo vector no lo dispara — `lumen_db_params(a, b, ...)`
(variádica, con un *fold expression*) es exactamente eso, y con ella el mismo código compila limpio.
Documentado aquí porque es la clase de hallazgo que un futuro cambio de compilador podría revertir
sin previo aviso: si algún día esto vuelve a fallar de la misma forma, la causa ya está identificada.

*Verificación.* `bench/lumen/app.lum` pasa de 4 a 10 rutas nativas: las seis que tocan `sqlite` con
`query`/`last_id` sin transacción (`/users/:id`, `/users`, `/products/:id`, `/products`,
`/orders/:id`, `/stats/sales`) se suman a las cuatro de antes. Verificado con las dos vías
compitiendo lado a lado, con el `seed.db` real del banco de pruebas (5000 usuarios, productos y
pedidos reales) sirviendo HTTP de verdad: paginación, `JOIN`s de tres tablas, `GROUP BY`/`SUM`/
`COUNT` con `ORDER BY`/`LIMIT`, objetos anidados construidos a mano (`{"user": {...}, "product":
{...}}`) y los casos de error (404 de fila inexistente, 400 de paginación inválida) — respuesta
IDÉNTICA, byte a byte, en las dos vías, en cada caso. `POST /users`/`POST /products`/`PUT
/products/:id` siguen en bytecode (el parámetro es una clase, con o sin campos opcionales — fuera de
esta fase, ver Fase 5.6 para por qué `POST /orders` sigue así pese a que ya soporta
`begin()`/`commit()`); `/counter/*`/`/payload/:n` siguen en bytecode por razones ajenas a esto (no relacionadas
con `sqlite`). `tests/native_route_shadow.cpp` gana una ruta (`/db/:id`) que solo comprueba que
COMPILA como asíncrona (vía `rutas_informe`, Fase 6) — ejecutarla en la prueba aislada, sin un
*event loop* real, se quedaría colgada para siempre (`DbAwaitable`, a diferencia de
`SleepAwaitable`, no tiene atajo sin *loop*: SIEMPRE reanuda desde el hilo del `DbPool` vía
`loop->post(...)`) — la ejecución de verdad es la de arriba, contra el binario real. 79/79 del
corpus y las 8 suites de `ctest` en verde.

### Fase 5.6 — `await <módulo>.begin()`/`commit()`/`rollback()`

Pedido explícitamente después de comprobar, con un benchmark real, que la Fase 5.5 dejaba fuera
justo las rutas que más lo necesitan: cualquier ruta que tocara una transacción (típicamente
cualquier escritura no trivial — reservar stock y crear un pedido a la vez, por ejemplo) caía
entera a bytecode, sin importar lo simple que fuera el resto de su cuerpo. La discusión que motivó
esto: en SQLite el propio motor (E/S + bloqueo) domina el tiempo de una consulta, así que
`--native` no acelera la consulta en sí — pero SÍ elimina el "precipicio" de caer a bytecode en
cuanto una ruta abre una transacción, que es la mayoría de las rutas de escritura reales. Con esto,
la cobertura de `--native` deja de depender de si una ruta hace `begin()`, solo de si usa algo
todavía no representable (una clase como parámetro de cuerpo, principalmente).

*Lo mínimo que faltaba: tres `native_id` más, cero infraestructura nueva.* `begin()`/`commit()`/
`rollback()` son, en `natives.cpp`, la misma clase de llamada que `last_id()` — un `DbModuleCall`
sin argumentos (`min_args = max_args = 1`, donde ese `1` cuenta el nombre del módulo implícito, no
ningún argumento real de `.lum`) — así que `Comprobador::tipo_provable()` los reconoce con el mismo
patrón que ya tenía `last_id()`: cero argumentos, `usa_await_ = true`, `Type::json()` de vuelta
(igual que los demás: éxito da `Value::boolean(true)`, fallo da `{"error": ...}`, ver
`await_db()` en `db.cpp`). El codegen (`Generador::expr`, caso `Await`) extiende el `switch` de
`DbOp` que ya tenía Query/Exec/LastId con Begin/Commit/Rollback — mismo `co_await
lumen_script::await_db(...)`, sql/params vacíos.

*Lo que sí era trabajo de verdad: cerrar una transacción que el handler deja abierta.* Bytecode ya
resolvía esto con `rollback_pendientes()` (`project.cpp`): tras que el VM termine, si quedó algún
`pinned_workers` sin `commit()`/`rollback()`, lo deshace con un `ROLLBACK` directo — necesario
porque, sin eso, la conexión que abrió la transacción quedaría fijada a medio camino para siempre,
y el siguiente que la reutilizara heredaría ese estado. Para que `--native` tenga la MISMA garantía
sin reimplementar la lógica aparte (la disciplina de toda esta fase: un solo camino, no dos que
puedan divergir en silencio), esa función se extrajo a `lumen_script::rollback_pendientes_db()`
(`db.hpp`/`db.cpp`, tomando `pinned_workers`+`EventLoop*` por referencia en vez de `NativeCtx`+
`Request` enteros) — `rollback_pendientes()` de bytecode pasa a ser un adaptador de una línea sobre
esto, igual que ya le pasó a `run_db()`/`await_db()` en la Fase 5.5.

El problema real es DÓNDE llamarla desde código nativo. Bytecode tiene un único punto de salida (el
`while (Suspended)` de `build_routes()` termina, y ahí, una sola vez, se llama a la limpieza antes
de construir la respuesta). Una ruta nativa no: cada `return` del cuerpo `.lum` se traduce en un
`return`/`co_return` de C++ en el punto exacto donde aparece, potencialmente muchos por ruta, y
C++ no tiene `finally`. La solución fue centralizar en el único sitio por el que YA pasa cualquier
salida temprana: `Generador::ret_vacio()` (el que decide `return;` vs `co_return;`) es, por
construcción, el punto de paso obligado de todo `return`/`require ... else ...`/400 de parámetro
inválido dentro de una ruta — así que ahora, si `Comprobador::usa_transaccion()` (puesto a verdad
en el mismo sitio que `usa_await_`, solo para `begin()`) dio verdad, `ret_vacio()` antepone `co_await
lumen_script::rollback_pendientes_db(l_pinned_workers, req.loop);` antes del `co_return;`. El único
punto de salida que `ret_vacio()` NO cubre — la caída natural al final del cuerpo, sin ningún
`return` explícito (el 204 implícito) — se cubre aparte, con la misma llamada, en
`generar_ruta_nativa()` justo antes de ese `res.status(204).send("")`. Un caso concreto lo prueba:
`tests/native_route_shadow.cpp` añade `/db/tx/:id`, con un `return status(400)` a propósito ANTES
del `commit()` — exactamente el caso que ejercita la limpieza en un punto de salida temprano, no
solo al final.

Deliberadamente NO se limpia desde el `catch(...)` que ya envuelve toda ruta nativa (la corrección
de la Fase 4): una excepción sin atrapar dentro de una transacción abierta ya es, hoy, el mismo
hueco en bytecode (`build_routes()` tampoco llama a `rollback_pendientes()` en su rama
`VM::Status::Error`) — y este documento existe justo para que las dos vías no diverjan en qué
limpian y qué no, no para que `--native` sea silenciosamente "mejor" en un punto que bytecode no
cubre. Si algún día se corrige en bytecode, corregirlo aquí también es el mismo cambio de una
línea.

*Verificación.* `tests/native_route_shadow.cpp` gana `/db/tx/:id` (`begin()`/`exec()`/`commit()`,
con el `return` anticipado ya descrito) — igual que `/db/:id` en la Fase 5.5, solo se comprueba que
COMPILA como ruta asíncrona (`rutas_informe`), no se ejecuta en la prueba aislada (mismo motivo:
`DbAwaitable` necesita un *event loop* real). La ejecución de verdad se hizo con una app suelta
contra el `seed.db` real del banco de pruebas: una ruta con `begin()` → `exec()` (resta *stock*) →
`query()` → `commit()` deja el descuento persistido — confirmado repitiendo la petición y viendo el
*stock* bajar de forma acumulativa entre peticiones distintas, no solo dentro de una — y otra con
`begin()` → `exec()` (resta 999999, deliberadamente disparatado) → `rollback()` deja el *stock*
intacto. Cinco peticiones concurrentes a la ruta de `commit()` dieron cinco decrementos
consecutivos sin ninguno perdido ni duplicado, confirmando que el *pinning* de conexión por
transacción sigue serializando correctamente bajo concurrencia real, no solo en el caso secuencial.
`bench/lumen/app.lum`/`bench/lumen-native/app.lum` no ganan ninguna ruta nueva en el informe de
arranque con esto — `POST /orders` sigue cayendo a bytecode, pero ahora por una razón distinta y ya
documentada (el cuerpo es una clase, ver Fase 3), no por `begin()`/`commit()` — verificado
comprobando a mano que, quitando el parámetro de cuerpo de una copia de esa ruta, sí compila nativa.
79/79 del corpus y las 8 suites de `ctest` en verde.

### Fase 5.7 — Parámetro de cuerpo: una clase, sin `validate:`

El otro hueco señalado en la Fase 4 ("un parámetro de tipo clase... deja la ruta entera sin
compilar") y confirmado con el benchmark real: `PUT /products/:id` (`ProductUpdate`, dos campos
`?`, sin `validate:`) seguía en bytecode pese a que el resto de la ruta —dos consultas SQL, una
comparación con `null`, un `update`— ya era representable desde la Fase 5.5/5.6. Alcance
deliberadamente acotado: una clase con **algún** `validate:` sigue cayendo a bytecode (ver más
abajo por qué), así que `UserIn`/`ProductIn`/`OrderIn` del banco de pruebas no se benefician
todavía — solo `ProductUpdate`.

*Campo `?` como tipo dinámico, no como hueco.* Hasta ahora `construir_clases()` excluía la clase
ENTERA si algún campo llevaba `?` — el layout de struct fijo (§7) no tenía dónde meter "puede ser
null". La solución reutiliza la Fase 5.5 en vez de inventar `std::optional<T>`: un campo `?`
almacena su valor como `Value` (igual que el resultado de una consulta), y `Comprobador` lo
expone con `Type::json()` en vez de su tipo escalar declarado — `datos.price` (`double? price`)
tiene, para el resto del compilador, EXACTAMENTE la misma pinta que `fila["price"]`. Un campo
NO opcional sigue siendo su tipo concreto tal cual, sin ningún cambio. `CampoNativo` guarda el
`Kind` escalar y la ortografía originales (`"int"`/`"long"`/`"float"`/`"double"`) aparte de esto,
solo para el mensaje de error del *binding* del cuerpo (ver más abajo) — un campo opcional sigue
siendo, de cara al usuario, un `double`, no un `Json`.

*`x == null`/`x != null`, la pieza que faltaba para poder USAR un campo opcional.* `datos.price
!= null` es la forma en que el `.lum` de verdad distingue "no vino" de "vino con valor" (ver
`PUT /products/:id` en `bench/lumen/app.lum`) — sin esto, un campo `?` recién representable no
serviría de nada. `NullLit` en sí mismo sigue sin ninguna representación fuera de este contexto
(`tipo_provable`, caso `NullLit`, sigue devolviendo `std::nullopt`); `Comprobador::tipo_provable`
mira el patrón `==`/`!=` contra un `NullLit` ANTES de pedir el tipo de los dos lados (pedírselo a
un `NullLit` directamente daría `std::nullopt` y tumbaría la rama entera) y solo lo admite si el
otro lado es Json-dinámico — un escalar nativo nunca es `null` en tiempo de ejecución, así que la
comparación, aunque compilase, sería siempre el mismo booleano constante: no vale la pena
representarla, mejor que se quede sin demostrar y avise con el resto de la ruta cayendo a
bytecode. El código generado es directo: `Value::is_null()`, sin pasar por `valor_json()` (el
lado `null` no tiene nada que convertir).

*El* binding *del cuerpo: reproducido a mano, no compartido con bytecode.* A diferencia de
`await_db()`/`rollback_pendientes_db()` (Fase 5.5/5.6), `bind_body()` (`project.cpp`) depende de
tipos que viven dentro de ese fichero y no se exponen (`ClassInfo`, el `Chunk` bytecode de una
regla `validate:`, la `VM` para ejecutarlo) — y las clases que SÍ llegan aquí (sin `validate:`,
ver el punto siguiente) nunca tienen ninguna regla que ejecutar, así que no hace falta ninguna de
esas piezas: el *binding* entero —parsear el JSON, campo por campo, obligatorio/opcional, tipo— es
C++ generado directo sobre `Value` (`codigo_bind_cuerpo()`), reproduciendo `bind_body()` mensaje a
mensaje, sin ninguna tabla en tiempo de ejecución de por medio. `L<Clase>` (Fase 3) no tiene
constructor por defecto —el único que genera `generar_clase_runtime()` toma TODOS los campos—, así
que cada campo vive en su propia variable C++ hasta que se sabe que ningún mensaje de error hizo
falta; solo entonces se construye la instancia, con los campos en el mismo orden que
`generar_clase_runtime()` ya fija.

*Encontrado verificando esto contra HTTP real, no en la prueba aislada: `on error 422:` leía un
hilo vacío.* La app puede declarar `on error 422:` (`bench/lumen/app.lum` lo hace, para dar
`{"error": "validacion", "detalles": error.messages}` en vez del cuerpo genérico) — ese manejador
lee `error.messages` de un `thread_local` (`lumen_script::last_validation_messages()`,
`natives.cpp`) que `bind_body()` rellena antes de responder 422. El primer intento de esta fase no
lo tocaba: el propio cuerpo 422 de la ruta nativa ya llevaba los mensajes correctos, pero SI la app
declaraba `on error 422:`, ese manejador sustituye la respuesta entera y leía lo que quedara de
una petición ANTERIOR en el mismo hilo (o nada). Confirmado comparando `PUT /products/:id` con un
campo de tipo equivocado contra las dos vías reales: bytecode daba `"detalles":["price: se esperaba
double"]`, `--native` daba `"detalles":[]`. Corregido en dos sitios, calcando exactamente lo que ya
hace `prepare_args()` (bytecode) para CUALQUIER ruta: `last_validation_messages().clear()` al
principio de TODA ruta nativa (no solo una con parámetro de cuerpo — una ruta que responda 422 a
mano sin pasar por aquí tampoco debe heredar mensajes de una petición ajena en el mismo hilo) y
`last_validation_messages() = mensajes` justo antes del 422 que sí lleva mensajes de campo. De
paso, hizo falta añadir `#include <lumen_script/natives.hpp>` a las cabeceras que
`compilar_nativo()` antepone siempre que hay rutas — no estaba, porque hasta ahora ninguna ruta
nativa necesitaba nada de ese fichero.

*Por qué una clase con `validate:` sigue fuera, en ESTA fase.* Ejecutar una regla `validate:`
significaría, con lo que hay hasta aquí, correr su `Chunk` bytecode dentro de una ruta por lo demás
compilada — mezclando VM y código nativo en el mismo *request*, o reimplementar la condición aparte
con el mismo riesgo de divergencia silenciosa que este documento entero existe para evitar. La vía
correcta (compilar la condición a IR, igual que el cuerpo de una ruta/función/método, y dejar que
`Comprobador`/`Generador` la traten como cualquier otra expresión booleana) es exactamente lo que
hace la Fase 5.8, justo a continuación — `ClaseNativa::tiene_validate` (renombrado ahí a
`reglas_ok`) deja la puerta marcada mientras tanto, rechazando limpiamente en vez de fingir que
compila.

*Verificación.* `bench/lumen-native/app.lum` gana `PUT /products/:id` en el informe de arranque
(11 rutas nativas, antes 10). Contra HTTP real, con las dos vías compitiendo lado a lado sobre el
mismo `seed.db`: actualización completa, solo `price`, solo `stock` (`price: null` explícito),
cuerpo vacío `{}`, JSON mal formado, cuerpo que no es un objeto, `price`/`stock` de tipo
equivocado (uno y los dos a la vez) y un id inexistente — respuesta IDÉNTICA byte a byte en los
nueve casos, incluida la lista de `"detalles"` del `on error 422:` de la aplicación.
`tests/native_route_shadow.cpp` gana una clase (`Ajuste`: un campo obligatorio, uno `?`, sin
`validate:`) y una ruta (`PUT /ajuste`) — a diferencia de `/db/:id`/`/db/tx/:id`, esta ruta no usa
`await` (compila SÍNCRONA), así que la prueba la EJECUTA de verdad (no solo comprueba que
compila) en ocho casos: cuerpo completo, campo opcional ausente, campo opcional `null` explícito,
campo obligatorio ausente, campo obligatorio de tipo inválido, campo opcional de tipo inválido,
JSON inválido, cuerpo que no es un objeto — bytecode y `--native` coinciden byte a byte en los
ocho. `pedir()` (el despachador de la prueba) gana un parámetro `body` opcional para poder
ejercitar esto. 79/79 del corpus y las 8 suites de `ctest` en verde.

### Fase 5.8 — `validate:` compilado a C++, y el modificador `.status(código)`

El incremento que la propia Fase 5.7 dejó marcado: una clase con `validate:` (`UserIn`/`ProductIn`/
`OrderIn` del banco de pruebas) seguía cayendo entera a bytecode. Con esto, `POST /users`/
`POST /products`/`POST /orders` pasan a compilar nativos — 14 de las 17 rutas del contrato, antes
11 (solo `/counter/*`/`/payload/:n` quedan fuera, por `SharedState`, sin relación con nada de esto).

*El canario que nadie usaba: `Emitter::check_condition()` ya devolvía el IR.* La Fase 1
("checker en paralelo") construyó `check_condition()` como contrapartida de `emit_condition()`
— cada regla de `validate:` ya se comprobaba DOS veces al compilar (una vez de verdad, con
`emit_condition()`, hacia el `Chunk` que ejecuta bytecode; otra en sombra, con `check_condition()`,
solo para comparar diagnósticos) — pero el `IrExprPtr` que la segunda pasada construye se
descartaba entero, `build_classes()` (`project.cpp`) solo lo usaba para el canario. Esta fase lo
captura de verdad: `construir_clases()` (`native_gen.cpp`) llama a `check_condition()` con los
campos de la clase como `names` (mismo orden que `emit_condition()`), y si no hay errores de sombra,
tiene un IR de la condición para compilar.

*Tener el IR no basta: hay que reprobarlo con el Comprobador de --native, no fiarse del checker de
bytecode.* `check_condition()` demuestra que la regla es válida para la VM — pero su noción de tipo
(`NombreTipado.tipo`, un string plano) no distingue un campo opcional de uno obligatorio, porque a
la VM no le hace falta: un `Value::null()` fluye igual de bien por cualquier ranura dinámica. Para
--native SÍ importa: un campo `?` es `Type::json()` (Fase 5.7), uno normal es su tipo escalar
exacto, y una regla que compare `precio >= 0` con `precio` opcional necesita el camino Json-dinámico
(Fase 5.5), no el numérico directo. Por eso cada clase con reglas arma su propio `Comprobador`
—registrado con `CampoNativo::tipo` de cada campo, consciente de Json donde toque— y vuelve a probar
el IR de `check_condition()` como `Type::Kind::Bool` con él. Sin funciones de usuario ni otras clases
visibles (`firmas_vacias`/`clases_vacias`): una regla real nunca las necesita (comparaciones,
aritmética, métodos de string como `.contains()`, ya soportados desde la Fase 2) — si alguna algún
día lo intentara, faltaría ahí y esta rama la rechazaría limpio, no a medias.

*Todo o nada, otra vez.* Si **cualquier** regla de una clase no demuestra `Bool` con este segundo
Comprobador, la clase ENTERA queda fuera como parámetro de cuerpo (`ClaseNativa::reglas_ok`) — nunca
"esta regla sí, esa no": ejecutar solo una parte de las reglas dejaría pasar datos que deberían haber
fallado una validación que `--native` se saltó en silencio, la misma familia de divergencia que este
documento entera existe para no permitir.

*El C++ se genera una sola vez, en `construir_clases()`, no en cada ruta que use la clase.* Cada
regla se traduce a texto C++ con `Generador::expr()` (un `Generador`/`Comprobador` dedicados a la
clase, sin ningún `Comprobador` de ruta de por medio) y se guarda ya lista
(`ReglaNativa::condicion_cpp`) — si dos rutas distintas tomaran la misma clase como cuerpo, las dos
reusarían el mismo texto sin recompilarlo. `Generador::expr()` traduce un `Ident` por NOMBRE
(`nombre_cpp(e.text)`, no por ranura — ver su propio comentario), así que el C++ resultante espera
encontrar una variable `l_<campo>` en el momento en que se evalúe: `codigo_bind_cuerpo()`
(Fase 5.7) las provee como **alias por referencia**, en un bloque propio, justo antes de evaluar las
reglas —solo si ningún campo dio mensaje ya, igual que `bind_body()`— y solo entonces construye la
instancia. El bloque propio no es cosmético: si la ruta tuviera OTRO parámetro con el mismo nombre
que un campo de la clase (`:precio` en el patrón y un campo `precio` en el cuerpo, por ejemplo), el
alias, con su propio *scope*, resuelve al campo correcto sin pisar nada de fuera.

*Lo que casi bloquea esto por completo: `.status(código)` encadenado sobre un valor.* Al verificar
`POST /orders`/`POST /users` contra el binario real (no solo la clase con `validate:` en sí, sino la
ruta ENTERA), las tres seguían en bytecode por una razón completamente distinta: `return { "error":
"stock insuficiente" }.status(409)` — un `DictLit` con un "modificador de respuesta" encadenado
(`natives.cpp`, `call_method()`: `status`/`header`/`cookie` fijan algo sobre `res` como efecto
lateral y devuelven el receptor SIN TOCARLO, para no reintroducir un objeto `response` mutable en el
lenguaje) — y esta fase no tenía ninguna representación para ese patrón, distinto de `es_llamada_respuesta()`
(que solo reconoce `status(N)` como llamada GLOBAL suelta, no un método encadenado sobre un valor).
Sin él, ninguna de las tres rutas de escritura del banco de pruebas llegaba a compilar, con o sin
`validate:`. Corregido extendiendo el mismo par que ya trata `DictLit`/`ListLit` en ambos lados
—`Comprobador::es_valor_json()` (comprobación: el argumento es `Int`, el receptor es
`es_valor_json()`) y `Generador::valor_json()` (generación: `(res.status(<código>), <valor_json del
receptor>)`, con el operador coma) — el orden de evaluación del operador coma, garantizado desde
C++17, asegura que el efecto lateral de fijar el código pasa ANTES de que el valor completo de la
expresión (el del receptor) se evalúe, así que es válido tanto en la posición más común (`return
X.status(N)`) como anidado dentro de un `DictLit`/`ListLit` más grande. Solo `.status()` está
cubierto — `.header()`/`.cookie()` son la misma idea, pero nada que este documento necesite compilar
todavía los usa; añadirlos, si hiciera falta, es el mismo patrón.

*Verificación.* `bench/lumen-native/app.lum` pasa de 11 a 14 rutas nativas en el informe de arranque:
`POST /users`/`POST /products`/`POST /orders` se suman. Contra HTTP real, con las dos vías
compitiendo lado a lado sobre el mismo `seed.db`: alta válida, regla de `validate:` incumplida (una
sola, las dos a la vez, con `.contains("@")` y con `and` entre dos comparaciones numéricas), campo
obligatorio ausente (nunca llega a evaluar las reglas), email duplicado (`.status(409)` sobre un
`DictLit` de error), producto con precio negativo, pedido con `quantity <= 0`, pedido a un producto
inexistente (404), pedido con stock insuficiente (`.status(409)` de nuevo, esta vez tras una
`sqlite.query()` real) y JSON malformado — respuesta IDÉNTICA byte a byte en cada caso. Cinco
peticiones concurrentes de `POST /orders` sobre el mismo producto dieron cinco `id` consecutivos sin
ninguno perdido, confirmando que la transacción (Fase 5.6) sigue serializando correctamente con
`validate:`/`.status()` de por medio. `tests/native_route_shadow.cpp` gana una clase (`Registro`: un
campo de tipo string y otro numérico, dos reglas —una con `.contains()` de tipo string, otra
numérica con `and`—) y una ruta (`POST /registro`, con `.status()` usado dos veces, en la rama de
éxito y en la de error) que se EJECUTA de verdad (sin `await`, igual que `/ajuste`) en seis casos:
camino feliz, una rama de negocio que no es un fallo de `validate:` (menor de edad, `.status(403)`),
cada regla fallando por separado, las dos a la vez, y un campo obligatorio ausente. 79/79 del corpus
y las 8 suites de `ctest` en verde.

### Fase 6 — Empaquetado y modo mixto
- `--native` de punta a punta: caché, `.so`, `dlopen`, informe de arranque, diagnóstico
  explícito de rutas no compilables, respaldo por bytecode ruta a ruta.
- **Aceptación:** una aplicación que use `ws`/`sse` (todavía no soportados nativamente) arranca
  con `--native`, sirve esas rutas por bytecode y el resto nativas, y lo dice al arrancar.

**El "respaldo por bytecode ruta a ruta" ya existía desde la Fase 4 — lo que faltaba era decirlo.**
El modo mixto en sí no es un mecanismo nuevo que construir: cada nivel (declarativo, nativo, VM) ya
se decide ruta por ruta, de forma independiente, desde que `build_routes()` ganó su "Nivel 1.5" — una
ruta que no compila a nativo simplemente no aparece en `NativeModule::rutas_por_indice`/
`rutas_async_por_indice`, y cae al `Nivel 2` (VM) con exactamente el mismo camino que tenía antes de
que `--native` existiera. Lo que sí faltaba era el **diagnóstico explícito**: el arranque solo daba
un conteo agregado, sin decir CUÁLES rutas son esas. Añadido con `Module::rutas_informe`
(`RutaInforme{metodo, patron, via}`, `via ∈ {declarativa, nativa, nativa (async), bytecode, ws,
sse}`), rellenado en cada uno de los seis puntos donde `build_routes()` ya decide/registra un
handler — no una comprobación nueva, solo anotar la decisión que cada rama ya toma. `main.cpp`, con
`--native`, imprime una línea por ruta con lógica (omite declarativas/`ws`/`sse`: esas nunca dependen
de si `--native` compiló algo). En el momento de escribir esto (antes del incremento de base de
datos que sigue más abajo), contra `bench/lumen/app.lum`: las diez rutas que tocan `sqlite` se
listaban `bytecode`, las tres puras `nativa`, y `/slow/:ms` `nativa (async)` — la utilidad de este
diagnóstico quedó demostrada enseguida, cuando pasó a hacer evidente, sin adivinar ni leer código,
exactamente CUÁLES de esas diez seguían sin compilar tras ese incremento y por qué. 79/79
del corpus y las 8 suites de `ctest` en verde.

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
