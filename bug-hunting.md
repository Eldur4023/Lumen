# Bug hunting

> Registro de los fallos reales que ha sacado probar Lumen contra su propio motor —no
> contra una versión instrumentada, no solo compilar— y de cómo se buscaron. Existe porque
> el patrón se repite demasiado como para no llevar cuenta: **compilar y enlazar no prueba
> nada de lo que importa.** Un módulo que no se ha ejecutado contra su motor no está escrito,
> está esbozado.
>
> Formato de cada entrada: qué se sospechaba, cómo se forzó, qué salió, el arreglo. Sin eso,
> "se encontró un bug" no es información — es la parte fácil.

---

## Metodología

Ordenada por lo que ha dado resultado hasta ahora, no por elegancia:

1. **Ejecutar contra el motor real, no contra un doble.** Los tres módulos de base de datos
   compilaban y enlazaban sin decir nada durante meses; los cuatro fallos de MySQL, el de
   postgres y el de sqlite solo aparecieron al lanzarlos contra un servidor de verdad.
2. **Aserciones sobre la forma de la salida, no solo el código de estado.** Un `200` con un
   cuerpo que ningún cliente puede parsear es peor que un error, porque el fallo aparece lejos
   de su origen. Comprobar "¿es JSON válido de verdad?" cazó tres bugs distintos (NaN de
   postgres, BLOB de sqlite y mysql, UTF-8 roto) que un test de solo-el-código-200 no ve.
3. **Sanitizadores, no solo pruebas funcionales.** TSan encontró la carrera de los búferes de
   *bind* compartidos entre workers del pool (406 apariciones en el informe) y la del apagado
   tocando los loops de otros hilos (14 apariciones) — ninguna de las dos se manifestaba en
   una ejecución normal.
4. **Leer el código sospechando, no confiando en el comentario de al lado.** El bug de
   multipart y el de `env()` salieron de auditar código, no de que algo fallara en
   producción — y el primero llevaba documentado como si funcionara desde el propio ejemplo
   del proyecto.
5. **Matriz sistemática de combinaciones**, en vez de fiarse de que "ya hay tests de eso". El
   punto 6 de esta lista.
6. **Fuzzing** — pendiente. Todo lo de arriba es adversarial dirigido por una persona; lo que
   no se le ha ocurrido a nadie probar, ninguno de estos métodos lo encuentra.

---

## Registro

### 2026-08-31 — postgres y mysql, a la altura de sqlite: lo que faltaba y lo que no aplicaba

`sqlite` tenía tres cosas que las otras dos baterías no: comprobación de JSON
válido en la fila de tipos, un caso de byte NUL dentro de un texto, y una
prueba de concurrencia que compara el *contenido* de cada respuesta, no solo
el código. Antes de copiar sin más, se leyó el driver de cada motor para
saber cuáles de esas tres aplican de verdad — copiar un test que no puede
fallar nunca no es cobertura, es ruido.

**Se aplicó:**

- **La concurrencia con contenido comprobado, en las dos.** Es la más
  importante de las tres: es exactamente la forma que tenía el fallo real de
  los búferes de *bind* compartidos en MySQL —una petición contestando con la
  fila de otra—, y la prueba de antes solo miraba el código `200`. Con el
  bug ya arreglado no iba a fallar hoy, pero si alguien lo regresara sin
  querer, la prueba de antes no se habría enterado.
- **`json_valido` en `/tipos`, en las dos.** `sqlite` ya lo tenía; faltaba en
  postgres y mysql, justo en la ruta que trae el tipo binario de cada motor
  —`bytea`, `BLOB`— que es exactamente el que ya rompió el JSON una vez.
- **El byte NUL dentro de un texto, en mysql.** Se leyó `db_mysql.cpp` antes
  de escribir nada: el valor se construye con
  `std::string(bufs[i].data(), std::min(lens[i], bufs[i].size()))`, que ya es
  seguro con bytes NUL de por medio — el arreglo del truncamiento de 1023
  bytes de esta misma sesión ya cubría esto de paso. No había ningún fallo
  que cazar, pero tampoco ninguna prueba que impidiera que alguien lo
  rompiera al tocar ese código sin saber por qué importa.
- **El valor del `bytea` de postgres, comprobado por primera vez.** La
  columna llevaba sembrada en el esquema desde que existe la batería, pero
  nadie afirmaba nada sobre ella — salía en el JSON de `/tipos` sin que
  ningún test la mirara. Sale como el hexadecimal propio de postgres
  (`\x68656c6c6f`), no como base64: libpq se lo entrega ya así y el driver no
  lo toca, así que es JSON-seguro de casualidad y no por diseño — la razón
  exacta que ya cita `README.md`. Ahora hay una prueba que lo afirma en vez
  de darlo por sabido.

**Se descartó, con el motivo comprobado, no supuesto:**

- **La caché de sentencias preparadas.** `sqlite` reutiliza una sentencia
  preparada entre llamadas —de ahí su prueba de que un `NULL` no se quede
  pegado del turno anterior—, pero ni `db_postgres.cpp`
  (`PQexecParams` directo, sin `PQprepare`) ni `db_mysql.cpp`
  (`mysql_stmt_init` + `mysql_stmt_prepare` en cada llamada) cachean nada: no
  hay sentencia que reutilizar, así que no hay riesgo que probar.
- **`NaN`/`Infinity` en mysql.** Comprobado contra un servidor real:
  `select 1e308 * 10` da `ERROR 1690: DOUBLE value is out of range`. Bajo
  `STRICT_TRANS_TABLES` —el modo por defecto— MySQL rechaza el valor antes de
  que llegue a existir; el bug que sí tiene postgres no tiene equivalente
  aquí.

`mysql` pasa de 26 a 29 pruebas; `postgres`, de 32 a 34. Total del proyecto:
246.

---

### 2026-08-31 — Primera campaña de fuzzing: lexer/parser/checker, HTTP y multipart

Sin libFuzzer —este toolchain es GCC, y `-fsanitize=fuzzer` es un builtin de
clang; instalarlo solo para esto habría sido una dependencia nueva para un
problema que se resuelve sin ella—. En su lugar, `fuzz/chaos.hpp`: mutación
"tonta" sin guía por cobertura (bit-flips, bytes al azar, inserciones,
borrados, empalmes) sobre semillas reales, un proceso hijo por caso para que
un `abort()` de ASan/UBSan tumbe solo ese caso. Detalle completo en
[fuzz/README.md](fuzz/README.md).

Tres objetivos, cada uno con `lumen_script`/`lumen` recompilados enteros bajo
`-fsanitize=address,undefined` —no solo el arnés, o el sanitizador no ve nada
del código real—:

| Objetivo | Casos | Fallos |
|---|---:|---:|
| Lexer + parser + checker (`lumen_script::compile()`, el mismo camino que `--check`) | 100.000 | 0 |
| Parser HTTP (llhttp de por medio, `HttpParser::feed()`) | 100.000 | 0 |
| Parser multipart (`parse_multipart()`) | 100.000 | 0 |

**Ningún crash, ningún cuelgue, en 300.000 casos.** Es una noticia real, no un
placebo: los tres objetivos estaban genuinamente sin fuzzear antes de hoy, y
el `alarm()` por caso sí habría cazado un bucle que no termina, no solo una
corrupción de memoria.

**Lo que esto NO cubre, para no leerlo como más de lo que es:** sin guía por
cobertura, la mutación tiende a quedarse cerca de las semillas —19 ficheros
`.lum` de `tests/casos` y media docena de peticiones HTTP escritas a mano—,
así que caminos de código que ninguna semilla toca casi no se visitan.
Tampoco prueba nada semánticamente *válido pero incorrecto* —un caso que
compila y no debería, o que compila a lo que no toca—, solo lo que cuelga o
corrompe memoria. Y no fuzzea los tres módulos de base de datos ni el motor
de plantillas todavía.

---

### 2026-08-31 — La matriz de enlace de parámetros entra en el repo, con control negativo

Consecuencia directa de la entrada de multipart de más abajo: el bug real es exactamente lo
que una matriz sistemática —origen (ruta / query / multipart) × tipo (escalar, `File`,
`List<File>`) × presencia (falta, mal tipado, en dos sitios a la vez)— habría encontrado sin
que nadie tuviera que sospechar de `save(dir)` primero. `tests/casos/parametros.lum` y once
casos nuevos en `tests/run_tests.sh` la cubren: texto junto a un fichero, los cuatro tipos
escalares como campo de formulario, texto junto a `List<File>`, el valor por defecto cuando
el campo falta en los dos sitios, la prioridad de la query sobre el formulario, y un escalar
mal tipado dentro de multipart.

**Control negativo**, para no dar por bueno "la prueba pasa" sin más: se reconstruyó el
binario con el `project.cpp` de antes del arreglo y se corrió solo esta sección contra él.
Fallaron exactamente las seis pruebas que tocan el patrón del bug —texto con fichero, los
tres tipos escalares, texto con `List<File>`— y ninguna otra; las que no dependen del enlace
nuevo (contar ficheros, caer al defecto, prioridad de la query) seguían pasando, como debía
ser. La suite mide lo que dice medir.

`regresion` pasa de 68 a 79 pruebas; el total del proyecto, de 230 a 241.

---

### 2026-08-31 — Los parámetros de texto nunca llegaban en `multipart/form-data`

**Se sospechaba** tras auditar `save(dir)`: el argumento del directorio no se sanea, solo el
nombre del fichero subido, así que si una app construye `dir` a partir de un parámetro de
texto del propio formulario (`imagen.save("./uploads/" + album)`, el patrón que documenta el
propio proyecto), ese parámetro podría ser un vector de *path traversal*.

**Se forzó** montando esa ruta exacta y subiendo un fichero con `curl -F`, variando el orden
de los campos.

**Salió** algo peor que lo que se buscaba: `album` llegaba **siempre vacío**, con o sin
intento de traversal, en cualquier orden. `prepare_args()` solo miraba la *query string* para
un parámetro escalar; nunca las partes de texto de un cuerpo multipart. `form()` tampoco
servía de rescate — solo lee `application/x-www-form-urlencoded`. No había ninguna forma de
leer un campo de texto de un formulario con ficheros.

**Arreglo:** antes de caer al valor por defecto, se busca en las partes del multipart una con
ese nombre y `filename` vacío (`src/lumen_script/project.cpp`, `prepare_args`). La *query string*
sigue teniendo prioridad si el nombre aparece en los dos sitios.

```
POST /avatar  (multipart: imagen=<fichero>, titulo=vacío)
→ antes:  {"titulo":""}          # siempre, pasara lo que pasara
→ ahora:  422 "titulo: obligatorio"   # si el handler lo valida, ahora puede
```

---

### 2026-08-31 — `env("VAR")` sobre una variable inexistente, sin ningún aviso

**Se sospechaba** que un despliegue que olvida `SESSION_SECRET` fallaría en abierto: secreto
vacío, cookies firmables por cualquiera.

**Se forzó** compilando un `app:` con `session: secret env("NO_EXISTE")` y mirando qué pasaba
al usar `session.*`.

**Salió** que el temor concreto era infundado —`secret.empty()` se trata exactamente igual
que "`session:` no configurado en absoluto", así que falla **cerrado**: toda operación de
sesión da error, `jwt.valid` siempre `false`— pero el diagnóstico real quedaba escondido: en
vez de un error claro al arrancar, lo que aparecía era *"la sesión no está configurada"* en
cada petición, sin mencionar la variable de entorno que faltaba.

**Arreglo:** aviso a `stderr` con fichero:línea:columna en el momento de compilar
(`src/lumen_script/parser.cpp`), sin bloquear la compilación —`.lum` no tiene por qué conocer el
entorno de despliegue final, y `--check` debe poder correr sin él.

```
lumen: aviso: app.lum:4:20: la variable de entorno 'SESSION_SECRET'
no esta definida; se usa "" en su lugar
```

---

### 2026-08-31 — Un cuerpo de más de 16 MB daba `400`, no `413`

**Se encontró** leyendo el parser HTTP: `kMaxBodySize` (16 MB, incluidas las subidas
multipart) existía y funcionaba, pero cualquier violación de límite del parser —cuerpo
demasiado grande incluida— se traducía en un `400 Bad Request` genérico. El código 413 ya
estaba en la tabla de razones de `Response`, sin usar para este caso.

**Arreglo:** el parser distingue el motivo exacto (`HttpParser::body_too_large()`); la
conexión responde `413 Content Too Large` específicamente para ese caso
(`src/http/http_parser.{hpp,cpp}`, `src/http/http_connection.cpp`).

---

### 2026-08-31 — El tope de pasos del VM no ve un bucle que se suspende sin avanzar

**Se encontró** leyendo el comentario del propio límite: se reinicia en cada suspensión, a
propósito, para que un SSE legítimo pueda vivir horas. Pero eso deja un hueco que el
comentario no cubre: `while true: await sleep(0)` se suspende y reanuda sin acumular pasos
entre dos suspensiones, así que el tope nunca lo ve. Un solo cliente podría fijar un hilo
entero reprogramando un temporizador de 0 ms sin parar.

**Arreglo:** piso de 1 ms en todo `sleep()`, en los tres sitios donde el motor reanuda un
handler —ruta normal, SSE, WS— (`src/lumen_script/project.cpp`, `clamp_sleep_ms`). No lo impide del
todo, pero lo acota a ~1000 reanudaciones/s por conexión en vez de tantas como el
planificador quiera dar. Verificado que no toca el caso legítimo: `sleep(500)` sigue tardando
500 ms exactos; `sleep(0)` pasa de 0 a ~1,3 ms.

---

### 2026-08-31 — El propio arnés de benchmark medía tres servidores a la vez

No es un bug de Lumen — de la medida. Se deja registrado porque el método que lo destapó es
el mismo de esta lista: no confiar en que un proceso muerto por `pkill` esté realmente
muerto.

**Salió al auditar** un `mezcla con escrituras` con 2.173 respuestas `500` que no cuadraban
con nada. Resultó ser `SO_REUSEPORT`: dos binarios de una prueba A/B anterior seguían vivos
en el mismo puerto —58 minutos después—, repartiéndose conexiones con el que se creía estar
midiendo, sin dar ningún error visible.

Segunda vuelta: arreglado el `pkill`, apareció `database disk image is malformed`. El
apagado ordenado de Lumen **deja de escuchar antes de terminar de drenar**, y el arnés
restauraba `datos.db` en cuanto el puerto quedaba libre, sobrescribiendo el fichero bajo un
proceso que todavía lo tenía abierto.

**Arreglo (en el arnés, fuera del repo):** matar por *puerto*, no por nombre de proceso —
`pkill` no alcanzaba dos binarios con nombre distinto sirviendo lo mismo—, esperar con
`kill -0` a que el PID desaparezca de verdad, y restaurar la base con `rm` + `mv` atómico en
vez de `cp` en el sitio.

---

### 2026-08-31 — MySQL: cuatro fallos que solo se veían al ejecutarlo

Compilaba y enlazaba desde hacía tiempo; nunca se había lanzado contra un servidor real hasta
esta ronda.

1. **Las transacciones no abrían.** `BEGIN` no es preparable por el protocolo de sentencias
   preparadas de MySQL (error 1295); el error se descartaba en silencio, así que `ROLLBACK`
   devolvía éxito sin haber deshecho nada. Arreglo: `mysql_real_query` para sentencias sin
   parámetros.
2. **Truncamiento silencioso a partir de 1023 bytes.** Faltaba
   `mysql_stmt_attr_set(STMT_ATTR_UPDATE_MAX_LENGTH)` + `mysql_stmt_store_result()` antes de
   leer `field.max_length`.
3. **`BIGINT UNSIGNED` grande se clavaba en `INT64_MAX`.** El valor sin signo no se
   distinguía del con signo al convertir.
4. **Búferes de *bind* compartidos entre los N workers del pool.** Solo visible con
   ThreadSanitizer: 406 apariciones del fichero en los informes de carrera, cero después de
   mover el estado de *bind* a variables locales por llamada.

---

### 2026-08-31 — Postgres: `NaN`/`Infinity` rompían el JSON de salida

**La misma batería, pasada a postgres**, sacó un fallo que no era del módulo sino del
serializador: postgres admite `NaN` e `Infinity` en un `double precision` y salían escritos
tal cual —`{"d":inf}`—, que ningún cliente JSON sabe leer. Arreglo: se escriben como `null`,
igual que `JSON.stringify`. De paso se corrigió la guía, que prometía un `last_id()` que
postgres no tiene —el driver ya lo rechazaba con un mensaje correcto; era la documentación la
que mentía.

---

### 2026-08-31 — sqlite y MySQL: un `BLOB` no es texto

El tercero de la misma familia: un `BLOB` se devolvía como cadena, así que sus bytes crudos
rompían la validez UTF-8 de la respuesta entera. Arreglo: base64 en los dos motores.
Postgres se libraba por casualidad —libpq entrega `bytea` ya en hexadecimal.

Los tres fallos de esta y las dos entradas anteriores comparten un patrón: **el módulo
contestaba `200` con un cuerpo que ningún cliente puede leer.** No falla la petición, falla
quien la recibe, lejos del origen — por eso merece la pena forzarlo a propósito y no confiar
en que "si compiló y dio 200, está bien".

---

### 2026-08-31 — UTF-8 inválido alcanzable desde la red por tres vías

Se creía un caso raro de sqlite; resultó alcanzable desde cuerpo JSON, *query* y cabeceras.
Un cliente mandaba un byte suelto y Lumen contestaba `200` con una respuesta que el propio
cliente no podía leer. Arreglo: una pasada de validación en la salida, aparte del bucle de
escapado existente, con el mismo atajo de ASCII de 8 bytes para no pagar el coste en el caso
común. Medido con tres repeticiones alternadas: **+0,1 % en un JSON de 57 KB y −1,5 % en el
detalle de 2 KB** — las primeras medidas de una sola pasada decían −5,9 % / −10,1 % y eran
ruido de medición, no el coste real.

---

### 2026-08-31 — El apagado ordenado tocaba los loops de otros hilos

Encontrado con ThreadSanitizer: 14 apariciones del fichero en los informes de carrera durante
el apagado con conexiones abiertas. Arreglo: cada hilo deja de aceptar mirando solo su propio
estado.

---

## Pendiente

- **Fuzzear los módulos de base de datos y el motor de plantillas.** Los tres objetivos de
  hoy no los cubren — ver el resultado de la campaña, arriba.
- **Más semillas para `fuzz_lenguaje`.** Sin guía por cobertura, la mutación no se aleja
  mucho de las 19 que tiene hoy (`tests/casos`); los programas completos de
  `LUMEN_SCRIPT-GRAMMAR.md` u otras construcciones raras servirían de semilla adicional.
- **Repetir la campaña de vez en cuando, no una sola vez.** Sin cobertura no hay "ya está
  fuzzeado" — cada corrida nueva visita un camino distinto por puro azar.
- **La precisión de `numeric` en postgres.** `t_numeric` se guardó como
  `12345678901234.1234567890` y salió `12345678901234.123`: el OID 1700 se convierte con
  `strtod`, que es `double`, no precisión arbitraria. Es una simplificación conocida —hay
  un tipo `float`/`double` en Lumen Script y no uno decimal—, no un fallo de conversión, pero
  tampoco está documentada como límite en ningún sitio.
- **Los ejemplos de la documentación, compilados y ejecutados en CI.** El bug de multipart
  llevaba documentado como si funcionara desde el propio ejemplo del proyecto — nadie lo
  había vuelto a compilar desde que se escribió.
