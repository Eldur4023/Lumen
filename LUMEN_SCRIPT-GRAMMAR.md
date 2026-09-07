# Lumen Script — El lenguaje

> Manual de referencia completo: léxico, tipos, gramática y semántica de Lumen Script, el lenguaje
> que compila y sirve Lumen 2.0. Sustituye a los ficheros `ejemplo-*.lum` y `prueba-*.lum`
> que antes vivían en la raíz del repositorio — lo que enseñaban por demostración, este
> documento lo enseña explicado, con la misma gramática pero cero ambigüedad sobre qué es
> parte del lenguaje y qué era solo un capricho de quien escribió el ejemplo.
>
> Notación de la gramática formal: `::=` producción, `|` alternativa, `[ ]` opcional,
> `{ }` cero o más repeticiones, `( )` agrupación, MAYÚSCULAS un terminal que produce el
> lexer, `"texto"` un literal exacto. Todo bloque de código sin marcar es Lumen Script válido y
> compilable tal cual — nada de lo que sigue es pseudocódigo.
>
> Para la guía práctica de cómo montar una aplicación —estructura de proyecto, cómo se
> arranca, qué prueba el binario de sí mismo— está [GUIDE.md](GUIDE.md). Para las decisiones
> de diseño y sus motivos, [README.md](README.md#decisiones-de-diseño). Este documento es el
> tercero: el lenguaje en sí, de arriba abajo, sin dar nada por sabido.

---

## Índice

**Parte I — Léxico**
[1](#1-comentarios-y-bloques) · [2](#2-palabras-y-objetos-reservados) · [3](#3-identificadores)
· [4](#4-literales) · [5](#5-cadenas) · [6](#6-la-trampa-de-)

**Parte II — Tipos**
[7](#7-primitivos) · [8](#8-clases-nativas) · [9](#9-opcionalidad) · [10](#10-genéricos)
· [11](#11--de-tipo-frente-a-ternario)

**Parte III — Estructura de un programa**
[12](#12-ficheros-y-compilación-en-dos-pasadas) · [13](#13-import) · [14](#14-app)
· [15](#15-class) · [16](#16-fn) · [17](#17-rutas) · [18](#18-parámetros-de-ruta)
· [19](#19-group) · [20](#20-on-error)

**Parte IV — Sentencias**
[21](#21-declaración-de-variables) · [22](#22-asignación) · [23](#23-if--elif--else-if--else)
· [24](#24-while) · [25](#25-for) · [26](#26-return) · [27](#27-require--else)
· [28](#28-trycatch) · [29](#29-break-y-continue) · [30](#30--y---como-expresiones)

**Parte V — Expresiones**
[31](#31-precedencia-completa) · [32](#32-ternario) · [33](#33-booleanos-y-veracidad)
· [34](#34-igualdad-y-comparación) · [35](#35-aritmética-sin-coerción)
· [36](#36-listas-y-diccionarios-literales) · [37](#37-indexación)
· [38](#38-llamadas-argumentos-posicionales-y-nombrados) · [39](#39-encadenado-postfijo)
· [40](#40-await-y-asincronía)

**Parte VI — Comprobado al compilar, no al ejecutar**
[41](#41-métodos-campos-y-tipos-se-resuelven-al-compilar) · [42](#42-inferencia-bidireccional)

**Parte VII — Retorno de un handler**
[43](#43-formas-de-return) · [44](#44-encadenado-status-header-cookie)

**Parte VIII — Objetos reservados**
[45](#45-la-superficie-completa) · [46](#46-los-módulos-de-base-de-datos)

**Parte IX — Ambigüedades y decisiones del parser**
[47](#47-tabla-de-ambigüedades)

**Programas completos**
[48](#48-tres-programas-completos)

**Apéndices**
[A](#apéndice-a--gramática-formal-completa) · [B](#apéndice-b--palabras-reservadas)
· [C](#apéndice-c--mensajes-de-error-citados-en-este-libro)

---

# Parte I — Léxico

## 1. Comentarios y bloques

Un comentario empieza en `#` y llega hasta el final de la línea. No hay comentarios de bloque
— si hicieran falta para una cadena larga, esa cadena probablemente debería ser una cadena de
tres comillas (§5).

```lum
# esto es un comentario
int x = 1   # y esto también
```

Los bloques se marcan por indentación, como en Python: el lexer emite tokens `INDENT` y
`DEDENT` comparando la sangría de cada línea con la de la anterior, y un bloque es la
secuencia de sentencias con mayor indentación que la línea que lo abre.

```lum
if x > 0:
    log.info("positivo")
    if x > 100:
        log.info("y grande")
    log.info("fin del primer nivel")
log.info("fuera del if")
```

**Solo espacios.** Una línea que empieza por tabulador no se traduce a un número de columnas
equivalente — eso exigiría decidir cuánto vale un tabulador, y cualquier valor que se elija es
una adivinanza. El lexer trata los espacios literalmente.

No hay `;` de fin de sentencia ni `{ }` de bloque. `{` y `}` están libres para un uso: el
literal de diccionario (§36).

---

## 2. Palabras y objetos reservados

Palabras reservadas del lenguaje — no se pueden usar como identificador:

```
import  class  fn  app  group  endpoint  on  error  origins
get  post  put  patch  delete  any  sse  ws
if  else  elif  while  for  in  return  require  try  catch  break  continue
validate  and  or  not  true  false  null  this  void  spa
```

Aparte están los **objetos reservados**: no se declaran, existen solo dentro del contexto que
los define, y usar uno fuera de sitio es un error al compilar, no una referencia a `null` en
producción.

| Objeto | Disponible en |
|---|---|
| `request` | cualquier handler |
| `session` | cualquier handler |
| `state` | cualquier handler y función |
| `jwt` | handlers bajo una guarda `jwt` |
| `sse` | handlers `sse` |
| `ws` | handlers `ws` |
| `error` | bloques `on error` (`code`, `message`, `messages`) |
| `log` | en todas partes |
| `this` | métodos y constructores |

```
./app.lum:12:9: error: 'sse' solo existe dentro de una ruta sse
```

---

## 3. Identificadores

```
IDENT ::= ( letra | "_" ) { letra | dígito | "_" }
```

`letra` incluye Unicode: `contraseña`, `título`, `año` son identificadores válidos —Lumen Script se
escribe en español tan a menudo como en inglés, y el lexer no obliga a transliterar.

```lum
class Usuario:
    string  nombre
    string? contraseña
    int     año_de_alta
```

---

## 4. Literales

```
INT      ::= dígito { dígito }
FLOAT    ::= dígito { dígito } "." dígito { dígito }
BOOL     ::= "true" | "false"
NULL     ::= "null"
```

No hay notación científica (`1.0e308` no lexa) ni separadores de millar (`1_000_000` no
lexa). Un entero no cabe en un punto decimal implícito: `1.` y `.5` no son literales válidos,
hace falta un dígito a cada lado del punto.

`true`, `false` y `null` van en minúscula — es Lumen Script, no C++, y desde luego no
`prueba.lum`, el primer boceto del lenguaje, que usaba `True`/`False` con mayúscula al
estilo Python y no llegó a compilar nunca contra el Lumen Script real.

---

## 5. Cadenas

```
STRING ::= '"' { carácter } '"' | '"""' { carácter | salto de línea } '"""'
```

Escapes, iguales en las dos formas: `\n` `\t` `\r` `\0` `\"` `\\`.

```lum
string saludo = "hola\tmundo\n"
```

### Cadenas de tres comillas

Para SQL o HTML, donde pelearse con `\n` en cada línea sería ruido puro:

```lum
string q = """
    SELECT id, titulo
    FROM posts
    WHERE autor = ?
    """
```

El margen **no** forma parte de la cadena — es indentación del fichero fuente, no del texto
que se quiere escribir. Se quita con tres reglas:

1. Un salto de línea justo detrás de la comilla de apertura no cuenta.
2. Si la comilla de cierre va sola en su línea, esa línea tampoco cuenta.
3. Del resto se quita la sangría **común** a todas las líneas con contenido — con lo que la
   sangría *relativa* entre ellas se conserva.

El ejemplo de arriba vale exactamente `SELECT id, titulo\nFROM posts\nWHERE autor = ?`, sin
salto de línea al principio ni al final. Con sangría relativa:

```lum
string html = """
    <ul>
      <li>uno</li>
      <li>dos</li>
    </ul>
    """
```

vale `<ul>\n  <li>uno</li>\n  <li>dos</li>\n</ul>` — la línea de `<ul>` pierde su margen común
de 4 espacios y las de `<li>` se quedan con los 2 espacios de más que tenían respecto a ella.

Solo se miran espacios: una línea que empieza por tabulador dentro de la cadena deja el
margen calculado en cero y no se recorta nada de esa cadena, en vez de adivinar cuánto vale un
tabulador y arriesgarse a acertar mal.

---

## 6. La trampa de `>>`

El lexer **no fusiona** dos `>` consecutivos en un token de desplazamiento a la derecha, así
que un genérico anidado cierra sin espacio de separación:

```lum
Dict<string, List<int>> por_categoria
```

C++ arrastró este bug hasta C++11, donde `vector<vector<int>>` era un error de sintaxis y
había que escribir `vector<vector<int> >` con un espacio. Lumen Script lo evita desde el primer día
simplemente no creando nunca el token `>>` — el lexer nunca combina dos operadores en uno sin
que haya un motivo léxico real para hacerlo.

---

# Parte II — Tipos

## 7. Primitivos

```
primitive ::= "int" | "long" | "float" | "double" | "bool" | "string" | "void"
```

Van en minúscula, siempre. `void` solo es válido como tipo de retorno de una `fn` que no
devuelve nada (§16).

**Un detalle que no está documentado en ningún otro sitio: `int` y `long` son el mismo tipo
por dentro.** El valor se guarda en un entero de 64 bits con signo tanto si el campo se
declaró `int` como si se declaró `long`; lo mismo pasa con `float` y `double`, que comparten
representación en coma flotante de doble precisión. Las cuatro palabras existen porque el
código que se porta desde otro lenguaje suele venir escrito con una de las dos, y forzar a
reescribirlo sería ceremonia sin beneficio — pero a efectos de rango y de precisión, elegir
`int` en vez de `long`, o `float` en vez de `double`, no cambia nada.

```lum
int    a = 9223372036854775807   # cabe, es un long long por dentro
long   b = 5                     # exactamente el mismo tipo que 'a'
float  c = 3.14159265358979      # doble precisión, aunque se llame float
double d = 3.14159265358979      # idéntico a 'c'
```

## 8. Clases nativas

| Tipo | Qué es |
|---|---|
| `Json` | Datos dinámicos: un cuerpo sin esquema, un mensaje de WebSocket, un literal de respuesta |
| `List<T>` | Lista homogénea |
| `Dict<K,V>` | Mapa homogéneo |
| `File` | Fichero subido: `name`, `filename`, `content_type`, `size`, y el método `save(dir)` |

```
generic_type ::= IDENT "<" type { "," type } ">"
```

En la práctica, la única clave de `Dict` que existe es `string` — y no por convención: el VM
lo comprueba en tres sitios (construir el literal, leer con `[ ]`, escribir con `[ ]`) y
cualquier otra cosa es un error en ejecución:

```
{"error":"la clave de un Dict tiene que ser string, no int"}
```

Los genéricos son **borrados**: el checker verifica `List<T>` y `Dict<K,V>` al compilar y el
tipo desaparece antes de emitir bytecode. El VM nunca ve un `List<File>` en ejecución, solo
una lista — es la misma técnica que Java, y por el mismo motivo: los genéricos existen para
que el compilador vigile, no para que el intérprete cargue con información que ya usó.

**No hay clases genéricas de usuario en 2.0.** `class Caja<T>:` no es una declaración válida.
Es una limitación deliberada, no un descuido, y es aditiva: puede llegar en una versión
posterior sin romper nada de lo que ya compila hoy.

## 9. Opcionalidad

```
type ::= base_type [ "?" ]
```

`T?` marca que el valor puede faltar.

```lum
class Usuario:
    string  nombre
    string? apodo          # puede no venir
```

En una clase que recibe el cuerpo de una petición (§18), un campo **no** opcional que falta
en el JSON produce automáticamente un `422`, sin que el handler llegue a ejecutarse — el
checker sabe qué campos son obligatorios y el runtime los exige antes de construir el objeto.
Uno opcional que falta simplemente queda a `null`:

```
POST /u  {"apodo":"x"}              -> 422 {"mensajes":["nombre: obligatorio"]}
POST /u  {"nombre":"ana"}           -> 200 {"nombre":"ana","apodo":null}
```

**La trampa heredada de Python:** con un valor opcional, `if apodo:` no distingue "vino y
vale una cadena vacía" de "no vino". Para preguntar por presencia hace falta comparar contra
`null` explícitamente:

```lum
if apodo == null:
    apodo = nombre
```

## 10. Genéricos

Cubierto en la práctica en §8. La regla formal:

```
base_type ::= primitive | generic_type | IDENT
```

Un `IDENT` en posición de tipo es el nombre de una `class` declarada en cualquier fichero del
proyecto — el orden entre ficheros es indiferente (§12).

## 11. `?` de tipo frente a ternario

`?` aparece en dos sitios de la gramática con significados distintos, y los dos son válidos
en la misma posición sintáctica:

```lum
string?  x = a ? b : c
```

Aquí el primer `?` marca `string` como opcional; el segundo abre un ternario. No es ambiguo,
pero exige que el parser mire un token por delante: tras un `?` en posición de tipo viene
siempre un `IDENT` (el nombre de la variable que se declara); en un ternario, tras el `?`
viene una expresión completa seguida de `:`. Un solo token de diferencia basta para
distinguir los dos casos sin retroceder.

---

# Parte III — Estructura de un programa

## 12. Ficheros y compilación en dos pasadas

```
program   ::= { top_level }
top_level ::= import_decl | class_decl | fn_decl | app_decl | group_decl
            | route_decl  | error_decl
```

El orden entre declaraciones **y entre ficheros** es indiferente. La compilación de un
proyecto entero ocurre en dos pasadas: primero se recorren todos los ficheros recogiendo qué
existe —clases, funciones, rutas—, y solo después se resuelven los nombres dentro de cada
cuerpo. Por eso una clase puede usarse antes de declararse, y una función puede llamar a otra
que aparece más abajo en el mismo fichero o en uno distinto (véase §16, `acumula`/`suma_hasta`
llamándose en orden inverso al de declaración).

## 13. `import`

```
import_decl ::= "import" IDENT NEWLINE
```

Introduce un espacio de nombres. Los módulos disponibles son `sqlite`, `postgres` y `mysql`
—los tres módulos de persistencia—, y cada uno necesita además su propio bloque de
configuración dentro de `app:` (§14, §46).

```lum
import postgres

app:
    postgres:
        host     "127.0.0.1"
        database "mi_app"
        user     "lumen_script"
        password env("PG_PASSWORD")
```

Cada módulo se compila dentro del binario de Lumen solo si su cliente nativo estaba presente
al compilar el propio Lumen. Importar uno que el binario no trae es un error **al compilar
el `.lum`**, no un fallo confuso la primera vez que se llama:

```
./app.lum:1:8: error: el binario no se compilo con soporte de 'postgres'
```

## 14. `app`

```
app_decl  ::= "app" ":" INDENT { app_entry } DEDENT

app_entry ::= "name"      STRING
            | "version"   STRING
            | "port"      INT
            | "templates" STRING
            | "static"    STRING "->" STRING [ "spa" ]
            | "docs" | "health" | "metrics"
            | IDENT ":" INDENT { config_pair } DEDENT

config_pair ::= IDENT expr NEWLINE
```

Puede estar en cualquier fichero del proyecto, pero **solo una vez** — no hay forma de partir
la configuración entre dos ficheros, a propósito: quien lee el proyecto por primera vez sabe
que hay un único sitio donde mirar.

```lum
app:
    name      "Mi aplicación"
    version   "1.0.0"
    port      8080
    templates "./templates"

    static "/static" -> "./public"
    static "/"       -> "./dist" spa

    docs                      # habilita /openapi.json y /docs
    health                    # habilita /health
    metrics                   # habilita /metrics, formato Prometheus

    session:
        secret  env("SESSION_SECRET")
        max_age 86400
        secure  true

    jwt:
        secret env("JWT_SECRET")
        issuer "mi-app"

    sqlite:
        file "./datos.db"
        pool 8
```

`env("VAR")` se resuelve **al compilar**, no al arrancar el proceso: es la única forma de que
un secreto no acabe escrito en el `.lum` que se versiona en git.

`spa` en un montaje estático hace que las rutas que no encuentran fichero caigan en
`index.html` — lo que necesita cualquier router del lado del cliente para que refrescar la
página en `/perfil/42` no dé un 404 del servidor de estáticos.

El bloque `IDENT ":" ...` genérico es lo que permite que `session:`, `jwt:`, `sqlite:`,
`postgres:` y `mysql:` compartan la misma forma sintáctica sin que la gramática tenga que
conocer cada módulo — cada uno interpreta sus propias `config_pair` después.

## 15. `class`

```
class_decl     ::= "class" IDENT ":" INDENT { class_member } DEDENT
class_member   ::= field | validate_block | constructor | method

field          ::= type IDENT NEWLINE
validate_block ::= "validate" ":" INDENT { validate_rule } DEDENT
validate_rule  ::= expr STRING NEWLINE
constructor    ::= IDENT "(" [ params ] ")" ( ":" block | NEWLINE )
method         ::= "fn" type IDENT "(" [ params ] ")" ":" block
```

### Campos y validación

```lum
class Usuario:
    int     id
    string  nombre
    int     edad
    string? contrasena          # ? = puede faltar

    validate:
        nombre != ""    "nombre: obligatorio"
        edad >= 0       "edad: no puede ser negativa"
        edad < 150      "edad: valor poco creible"
```

Cada `validate_rule` es una expresión booleana seguida del mensaje que se emite si es falsa.
**Todos los mensajes que fallan salen a la vez**, no solo el primero — un formulario con
cuatro errores recibe los cuatro de un tirón, en vez de obligar a corregir uno, reenviar,
descubrir el siguiente. Si alguna regla falla, el handler **no llega a ejecutarse**.

Las reglas de `validate` se compilan igual que cualquier otra expresión, así que un campo mal
escrito en una regla es un error de compilación y nunca llega a producción:

```
./app.lum:9:9: error: 'nombrre' no esta declarada
```

### Constructores

```lum
class Punto:
    int x
    int y

    Punto(int x):              # con cuerpo
        this.x = x
        this.y = 0

    Punto(int x, int y)        # sin cuerpo: cada parametro va a su campo, en orden
```

Se distinguen por el **número** de parámetros, no por su tipo — dos constructores con la
misma aridad son un error de compilación por ambiguos. Un constructor **sin cuerpo** asigna
sus parámetros a los campos homónimos en el orden en que se declararon; los campos que no
aparecen entre los parámetros quedan a `null` (solo válido si son opcionales). Sin ningún
constructor declarado, la clase recibe uno implícito con todos los campos, en orden:

```lum
class Caja:
    string nombre
    int    ancho
    int    alto

# ...

Caja c = Caja("grande", 3, 4)   # el constructor implícito
```

### Métodos

```lum
class Punto:
    int x
    int y

    fn int cuadrado():
        return this.x * this.x + this.y * this.y

    fn string etiqueta(string prefijo = "P"):
        return prefijo + "(" + str(this.x) + "," + str(this.y) + ")"

    fn Punto desplazado(int dx, int dy):
        return Punto(this.x + dx, this.y + dy)
```

Métodos y constructores se compilan como funciones normales con `this` de primer parámetro
implícito, así que comparten pila de marcos con `fn` y admiten recursión y valores por
defecto exactamente igual.

La llamada `p.cuadrado()` **se resuelve al compilar**, a partir del tipo declarado del
receptor — no hay despacho dinámico ni búsqueda en tabla de métodos en tiempo de ejecución.
Un método mal escrito no llega nunca a producción:

```
./app.lum:8:20: error: 'Punto' no tiene un metodo 'triple';
                  tiene cuadrado, etiqueta, desplazado
```

Una instancia construida a mano (`Punto(3, 4)`) y una enlazada del cuerpo de una petición
(`post endpoint("/p", Punto p)`) son exactamente el mismo tipo de valor: los métodos
funcionan igual sobre las dos.

## 16. `fn`

```
fn_decl ::= "fn" type IDENT "(" [ params ] ")" ":" block
params  ::= param { "," param }
param   ::= type IDENT [ "=" expr ]
```

```lum
fn int doble(int x):
    return x * 2

fn string saluda(string nombre, string tratamiento = "hola"):
    return tratamiento + ", " + nombre

fn int acumula(int n, int acc):
    if n <= 0:
        return acc
    return acumula(n - 1, acc + n)

fn int suma_hasta(int n):
    return acumula(n, 0)          # llama a una funcion declarada MAS ABAJO
```

Un parámetro sin valor por defecto no puede ir después de uno que sí lo tiene, igual que en
casi cualquier lenguaje con argumentos por defecto. Una función sin `return` explícito
devuelve `null`, y su tipo de retorno declarado debe ser `void` en ese caso.

La recursión funciona con un tope de 200 llamadas anidadas. Pasarse produce un error del
lenguaje —no un `stack overflow` del proceso, porque el VM tiene su propia pila (§40)— así que
un bug de recursión sin caso base se queda en un `500` legible, no en un binario que se cae:

```lum
fn int infinita(int n):
    return infinita(n + 1)
```

Un error dentro de una función se puede capturar donde se la llama:

```lum
fn int rompe(int n):
    return n / 0

get endpoint("/captura/:n", int n):
    try:
        return { "no": rompe(n) }
    catch e:
        return { "capturado": e.message }
```

Declarar una función con el mismo nombre que un builtin (`len`, `str`, `render`...) es un
error de compilación. Las funciones también se pueden llamar dentro de un bloque `validate:`:

```lum
fn bool es_correo(string s):
    return s.contains("@") and s.contains(".")

class Registro:
    string correo

    validate:
        es_correo(correo)   "correo: formato invalido"
```

## 17. Rutas

```
route_decl ::= method "endpoint" "(" STRING { "," param } ")" [ modifier ] ":" block
method     ::= "get" | "post" | "put" | "patch" | "delete" | "any" | "sse" | "ws"
modifier   ::= "origins" "(" STRING { "," STRING } ")"
```

```lum
get    endpoint("/ruta"):
post   endpoint("/ruta"):
put    endpoint("/ruta"):
patch  endpoint("/ruta"):
delete endpoint("/ruta"):
any    endpoint("/ruta"):                       # cualquier metodo
sse    endpoint("/ruta"):                       # flujo de eventos, implica GET
ws     endpoint("/ruta") origins("https://x"):  # WebSocket
```

El primer `STRING` es el patrón de ruta: `/usuarios/:id`, `/usuarios/{id}` (las dos formas
son equivalentes) y `/ficheros/*` para un comodín de cola. **El checker verifica las dos
direcciones**: que cada `:nombre` del patrón tenga un parámetro que lo recoja en la firma, y
que ningún parámetro declarado como segmento de ruta sobre sin sitio en el patrón.

```
./app.lum:4:1: error: el patron declara ':id' pero ningun parametro lo recoge
```

`origins(...)` solo es sintácticamente válido en `ws`, y ahí es **obligatorio** — la gramática
lo admite tras cualquier ruta, pero el checker lo rechaza fuera de una `ws` y rechaza una `ws`
que no lo lleve. Los navegadores no aplican la política de mismo origen al *handshake* de
WebSocket, así que sin una lista blanca cualquier página podría abrir la conexión desde el
navegador de un usuario y heredar sus cookies de sesión.

### Los dos niveles de ruta

Una ruta cuyo cuerpo entero se resuelve en tiempo de compilación —un único `return` de un
valor constante, o de una llamada nativa con solo argumentos literales— se convierte en una
**acción nativa**: una entrada en el radix tree de rutas que no ejecuta ni un solo paso de
bytecode.

```lum
get endpoint("/"):
    return render("index.html")        # declarativa: cero bytecode en cada peticion
```

El resto ejecuta bytecode sobre el VM propio (§40). Al arrancar, el binario informa de cuántas
rutas van por cada camino:

```
lumen: 3 fichero(s), 12 ruta(s) — 5 declarativa(s), 7 con logica
```

Una ruta con guardas de grupo heredadas **nunca** es declarativa, aunque su propio cuerpo lo
sería: la acción nativa no ejecutaría el `require` del grupo, y saltárselo sería un agujero de
seguridad con forma de optimización.

## 18. Parámetros de ruta

Todo lo que un handler necesita se declara en su firma — no hay un objeto `request` del que
extraer cosas a mano dentro del cuerpo, salvo para lo genuinamente ajeno al esquema
(`request.path`, `request.method`, `request.ip`, ver §45).

```lum
get endpoint("/usuarios/:id", int id, int page = 1, string q):
    return { "id": id, "page": page, "q": q }
```

| Forma del parámetro | De dónde sale |
|---|---|
| Nombre que aparece en el patrón (`:id`) | Segmento de la ruta |
| Nombre que no aparece en el patrón | Parámetro de la *query string*; si la petición es `multipart/form-data`, también un campo de texto del formulario con ese nombre |
| `= valor` | Valor por defecto si falta en los dos sitios anteriores |
| Tipo que es una `class` | Cuerpo JSON de la petición, validado antes de entrar al handler |
| `File` / `List<File>` | Una o varias partes `multipart/form-data` |

La *query string* tiene prioridad si el mismo nombre aparece en los dos sitios a la vez —caso
raro, pero determinista—. Esto es lo que permite mezclar ficheros y texto en el mismo
formulario sin nada especial en la firma:

```lum
post endpoint("/avatar", File imagen, string titulo):
    require titulo != "" else status(422)
    string nombre = imagen.save("./subidas")
    return { "nombre": nombre, "titulo": titulo }
```

Un valor de query, de ruta o de formulario que no encaja con su tipo declarado es un **400**,
no una excepción sin capturar:

```json
{"error":"parametro invalido","esperado":"int","param":"id","recibido":"abc"}
```

Con `File` (no `List<File>`), que falte el fichero es un `422`. Con `List<File>`, una lista
vacía no es error — el handler decide si eso le vale con un `require`.

## 19. `group`

```
group_decl   ::= "group" "(" STRING ")" ":" INDENT { group_member } DEDENT
group_member ::= require_stmt | route_decl | group_decl
```

```lum
group("/api/v1"):
    require jwt.valid else status(401)

    get endpoint("/yo"):
        return { "sub": jwt.claims["sub"] }

    group("/admin"):
        require jwt.claims["rol"] == "admin" else status(403)

        get endpoint("/stats"):
            return { "usuarios": state.get("usuarios", 0) }
```

Los prefijos se concatenan (`/api/v1` + `/admin` + `/stats` = `/api/v1/admin/stats`) y **las
guardas se acumulan**: para llegar a la ruta más anidada hace falta pasar primero la del grupo
padre y luego la propia, en ese orden — las guardas van siempre **antes** que las rutas dentro
de un bloque `group`, aunque en el fuente aparezcan entremezcladas con ellas.

No existe concepto de *middleware* en Lumen Script 2.0: `require expr else respuesta` (§27) es la
única forma de interceptar una petición antes de que llegue al handler, y es una expresión
condicional con una salida, no una cadena de funciones que se pueda componer arbitrariamente.
La razón está en [README.md](README.md#corre-detrás-de-un-reverse-proxy): CORS, compresión,
*rate limiting* y cabeceras de seguridad se delegan al proxy inverso a propósito, y lo único
que le quedaba al lenguaje por resolver era "¿puede esta petición seguir, o no?" — que es
exactamente lo que expresa `require`.

## 20. `on error`

```
error_decl ::= "on" "error" [ INT ] ":" block
```

```lum
on error 404:
    return render("404.html", ruta=request.path, metodo=request.method)

on error 403:
    return { "error": "no puedes entrar aqui", "codigo": error.code }

on error:
    log.error(error.message)
    return render("500.html")
```

Sin código, es el manejador global — el que atrapa cualquier estado de error que no tenga uno
más específico declarado. El código, si se da, tiene que estar entre **400 y 599**: con un
`2xx` el handler de la ruta ya ha escrito la respuesta, y sustituirla desde fuera sería un
filtro de respuesta — es decir, la mitad de lo que hace un *middleware*, que Lumen Script 2.0 evita a
propósito (§19).

Dentro del bloque existe el objeto reservado `error` (§45). En un `on error 422`,
`error.messages` trae la lista completa de mensajes de un `validate:` que falló —vacía si el
422 no vino de ahí—:

```lum
on error 422:
    return { "detalles": error.messages }
```

El código de estado de la respuesta se conserva tal cual, aunque el manejador cambie el
cuerpo. Si el manejador no escribe nada, se mantiene el cuerpo de error por defecto.

---

# Parte IV — Sentencias

```
block      ::= INDENT { statement } DEDENT
statement  ::= var_decl | assign_stmt | if_stmt | while_stmt | for_stmt
             | return_stmt | require_stmt | try_stmt
             | "break" NEWLINE | "continue" NEWLINE | expr NEWLINE
```

## 21. Declaración de variables

```
var_decl ::= type IDENT [ "=" expr ] NEWLINE
```

```lum
int    n = 5
string nombre
List<int> xs = [1, 2, 3]
```

Una variable declarada sin inicializador queda en su valor por defecto del tipo (`0` para los
numéricos, `""` para `string`, `false` para `bool`, `null` para un tipo opcional o de clase).

## 22. Asignación

```
assign_stmt ::= lvalue ( "=" | "+=" | "-=" | "*=" | "/=" | "%=" ) expr NEWLINE
lvalue      ::= IDENT { "." IDENT | "[" expr "]" }
```

```lum
n = n + 1
n += 1              # azucar de lo de arriba
obj.campo = 5
lista[0] = 99
mapa["clave"] += 1
lista[0] += 10       # compuesta tambien vale sobre un indice
```

`x += e` es exactamente azúcar de `x = x + e` — no hay una semántica distinta para tipos que
sobrecargan `+` (no hay sobrecarga de operadores en Lumen Script), así que `+=` sobre un `string`
concatena y sobre una `List` sirve para anexar los elementos de otra lista:

```lum
string s = "ho"
s += "la"                 # "hola"

List<int> l = [1]
l += [2, 3]                # [1, 2, 3]
```

## 23. `if` / `elif` / `else if` / `else`

```
if_stmt ::= "if" expr ":" block
            { ( "elif" | "else" "if" ) expr ":" block }
            [ "else" ":" block ]
```

`elif` y `else if` son intercambiables — la gramática acepta las dos formas, a propósito, para
no romper código escrito por quien viene de un lenguaje con una u otra convención:

```lum
get endpoint("/elif/:n", int n):
    if n < 0:
        return { "r": "negativo" }
    elif n == 0:
        return { "r": "cero" }
    else:
        return { "r": "grande" }

get endpoint("/elseif/:n", int n):
    if n == 1:
        return { "r": "uno" }
    else if n == 2:
        return { "r": "dos" }
    else:
        return { "r": "otro" }
```

No hay `else` colgante ambiguo: como los bloques son por indentación y no por `{ }`, un
`else` **siempre** cierra el `if` a su misma columna. El problema clásico de C
(`if (a) if (b) ...; else ...;`, ¿a qué `if` pertenece el `else`?) no existe porque no hay
forma de escribirlo de manera ambigua.

## 24. `while`

```
while_stmt ::= "while" expr ":" block
```

```lum
int total = 0
int k = 0
while k < 5:
    total += k
    k++
```

## 25. `for`

```
for_stmt ::= "for" type IDENT "in" expr ":" block
```

Recorre una `List<T>` por valor, o las **claves** de un `Dict<K,V>` (el tipo declarado en el
`for` debe coincidir con `K`, casi siempre `string`):

```lum
List<int> xs = [1, 2, 3, 4, 5]
int total = 0
for int x in xs:
    total = total + x

Dict<string,int> d = { "uno": 1, "dos": 2 }
List<string> claves = []
for string k in d:
    claves = claves + [k]
```

Solo hay una forma de `for` — no existe el `for (init; cond; incr)` de C ni un `range()`
aparte. Un bucle contado se escribe con `while` (§24) o recorriendo un rango construido a
mano; iterar sobre algo que no es `List` ni `Dict` es un error de compilación:

```
./app.lum:3:14: error: no se puede iterar sobre int
```

## 26. `return`

```
return_stmt ::= "return" [ expr ] NEWLINE
```

Cubierto en detalle en la Parte VII. `return` sin expresión sale con `204` cuando el tipo de
retorno de una `fn` es `void`; dentro de un handler, un `return` sin valor equivale a
`return status(204)`.

## 27. `require ... else`

```
require_stmt ::= "require" expr "else" expr NEWLINE
```

```lum
require imagen.content_type.starts_with("image/") else status(415)
require imagen.size <= 5 * 1024 * 1024             else status(413)
```

Azúcar exacto de:

```lum
if not (imagen.content_type.starts_with("image/")):
    return status(415)
```

No es un constructo de *middleware*: es un retorno anticipado como cualquier otro, y por eso
vale en **cualquier posición** donde valga una sentencia — dentro de un bucle, dentro de un
`if`, en medio de una función, no solo al principio de una ruta o un grupo (§19).

## 28. `try`/`catch`

```
try_stmt ::= "try" ":" block "catch" [ IDENT ] ":" block
```

```lum
get endpoint("/basico/:n", int n):
    try:
        int x = n / 0
        return { "no": x }
    catch e:
        return { "capturado": e.message }

get endpoint("/sin_variable/:n", int n):
    try:
        int x = 1 % n
        return { "ok": x }
    catch:
        return { "fallo": true }
```

El nombre tras `catch` es opcional; si se da, dentro del bloque `catch` es un valor con un
único campo útil, `.message`. Se resuelve con una tabla de rangos de bytecode calculada al
compilar, no con una pila de manejadores en tiempo de ejecución, así que:

- Un `return` **dentro** del `try` sale de la función con normalidad y no dispara el `catch`.
- Un `return` no puede dejar un manejador "colgado" para más tarde: en cuanto la ejecución
  sale del rango del `try` —por el camino que sea—, ese `catch` deja de estar activo.

  ```lum
  get endpoint("/no_fuga/:n", int n):
      int acc = 0
      for int i in [1, 2]:
          try:
              acc = acc + i
          catch e:
              acc = -1
      # Este fallo esta FUERA del try de arriba: tiene que reventar, no capturarse.
      int malo = acc / 0
      return { "no": malo }
  ```

- Anidados, gana el más interno:

  ```lum
  get endpoint("/anidado"):
      try:
          try:
              int x = 1 / 0
          catch e:
              return { "interno": e.message }   # este captura
      catch e:
          return { "externo": e.message }       # este no llega a verlo
  ```

- Se puede seguir ejecutando tras capturar, sin salir del handler:

  ```lum
  get endpoint("/sigue"):
      int total = 0
      for int d in [2, 0, 5]:
          try:
              total = total + 100 / d
          catch e:
              log.warn("saltado: " + e.message)
      return { "total": total }
  ```

Un error que no se captura en ningún `try` sube hasta convertirse en la respuesta de error por
defecto —o en el `on error` que aplique (§20)— con forma `{"error": mensaje, "en":
fichero:línea:columna}` y código `500`.

## 29. `break` y `continue`

```lum
for int x in [1, 2, 3, 4, 5]:
    if x == 3:
        break
    if x % 2 != 0:
        continue
    # ...
```

Solo válidos dentro de `while` o `for`. Usarlos fuera es un error de compilación, no un
efecto sin sentido en tiempo de ejecución.

## 30. `++` y `--` como expresiones

`++` y `--` **no son solo sentencias**: son operadores de expresión, con las dos formas
habituales, y valen sobre cualquier posición postfija — una variable, un campo, o un elemento
indexado.

```
unary   ::= [ "-" | "++" | "--" ] postfix
postfix ::= primary { "." IDENT | "(" [ args ] ")" | "[" expr "]" | "++" | "--" }
```

```lum
int i = 5
int a = i++          # a = 5, i = 6   -- postfijo: da el valor ANTES de incrementar
int b = ++i          # b = 7, i = 7   -- prefijo:  da el valor DESPUES
int c = i--          # c = 7, i = 6
int d = --i          # d = 5, i = 5
```

Sobre un campo:

```lum
class Caja:
    int n

get endpoint("/campo"):
    Caja c = Caja(10)
    int viejo = c.n++            # viejo = 10, c.n = 11
    int nuevo = ++c.n            # nuevo = 12, c.n = 12
    return { "viejo": viejo, "nuevo": nuevo, "n": c.n }
```

Sobre un índice de lista, encadenado con el propio `[ ]`:

```lum
List<int> l = [10, 20, 30]
int i = 0
int primero  = l[i++]       # primero = 10, i pasa a valer 1
int segundo  = l[i++]       # segundo = 20, i pasa a valer 2
```

Como sentencia suelta, el valor que produce simplemente se descarta:

```lum
i++
++i
i--
```

Y dentro de la condición de un `while`, exactamente igual que en C:

```lum
int total = 0
int k = 0
while k < 5:
    total += k++
```

---

# Parte V — Expresiones

```
expr      ::= ternary
ternary   ::= or_expr [ "?" expr ":" expr ]
or_expr   ::= and_expr { "or" and_expr }
and_expr  ::= not_expr { "and" not_expr }
not_expr  ::= [ "not" ] equality
equality  ::= comparison { ( "==" | "!=" ) comparison }
comparison::= sum { ( "<" | "<=" | ">" | ">=" ) sum }
sum       ::= product { ( "+" | "-" ) product }
product   ::= unary { ( "*" | "/" | "%" ) unary }
unary     ::= [ "-" | "++" | "--" ] postfix
postfix   ::= primary { "." IDENT | "(" [ args ] ")" | "[" expr "]" | "++" | "--" }

primary   ::= INT | FLOAT | STRING | BOOL | NULL
            | IDENT | "this" | "await" expr
            | dict_literal | list_literal
            | "(" expr ")"

args         ::= arg { "," arg }
arg          ::= expr | IDENT "=" expr
dict_literal ::= "{" [ dict_entry { "," dict_entry } ] "}"
dict_entry   ::= expr ":" expr
list_literal ::= "[" [ expr { "," expr } ] "]"
```

## 31. Precedencia completa

De menor a mayor — un ejemplo real por nivel:

| # | Operador | Asociatividad | Ejemplo |
|---|---|---|---|
| 1 | `?:` | derecha | `edad >= 18 ? "adulto" : "menor"` |
| 2 | `or` | izquierda | `a == 1 or b == 2` |
| 3 | `and` | izquierda | `a == 1 and b == 2` |
| 4 | `not` | (unario) | `not encontrado` |
| 5 | `== !=` | izquierda | `estado != "cerrado"` |
| 6 | `< <= > >=` | izquierda | `edad >= 18` |
| 7 | `+ -` | izquierda | `precio - descuento` |
| 8 | `* / %` | izquierda | `total * 1.21` |
| 9 | `-` (unario) | — | `-saldo` |
| 10 | `. ( ) [ ]` `++` `--` | izquierda | `pedidos[0].total()` |

`1 + 2 * 3` vale `7`, no `9` — la aritmética respeta la precedencia matemática habitual, y
`*`/`/`/`%` ligan más fuerte que `+`/`-` exactamente como en cualquier otro lenguaje con esta
familia de operadores.

## 32. Ternario

```lum
string rol = es_admin ? "admin" : "user"
```

Asociativo a la derecha y de la menor precedencia de todas — envuelve prácticamente cualquier
expresión sin necesitar paréntesis alrededor de las dos ramas:

```lum
return filas == 0 ? status(404) : status(204)
```

## 33. Booleanos y veracidad

`and`, `or` y `not` se compilan a saltos de bytecode con cortocircuito, igual que en Python o
C: en `a() and b()`, si `a()` es falso, `b()` **no se llega a evaluar**. Esto importa cuando
uno de los dos lados tiene efectos —una llamada a base de datos, un incremento— y no solo
cuando es una comprobación pura.

En contexto booleano (`if`, `while`, `require`, `and`, `or`, `not`, la condición de un
ternario) son **falsos**: `null`, `false`, `0`, `0.0`, la cadena vacía `""`, y la lista y el
diccionario vacíos. Todo lo demás es verdadero. Es la regla de Python, que es de donde viene
la mayoría de quien va a escribir Lumen Script por primera vez.

Ver también §9 para la trampa de `if x:` con un valor opcional.

## 34. Igualdad y comparación

**No hay coerción de tipos dentro de los operadores.** `0 == "0"` es `false` en Lumen Script, no
`true` como en JavaScript, y no un error de compilación tampoco — simplemente compara un
entero contra una cadena y el resultado es que no son iguales.

```lum
0 == "0"        # false, sin lanzar error
1 + "1"         # ERROR DE COMPILACION: no se puede sumar int y string
```

Para concatenar un número con texto hay que convertirlo explícitamente:

```lum
"n = " + str(n)
```

`<`, `<=`, `>`, `>=` solo están definidos entre tipos numéricos (`int`/`long`/`float`/`double`,
intercambiables entre sí, §7) y no entre `string`s — no hay orden lexicográfico de cadenas
integrado en los operadores de comparación.

## 35. Aritmética, sin coerción

`+`, `-`, `*`, `/`, `%` funcionan entre los tipos numéricos, y `+` además concatena dos
`string` o dos `List<T>` del mismo tipo de elemento. Cualquier otra combinación es un error
**al compilar**, no una conversión implícita silenciosa ni un `NaN` en tiempo de ejecución:

```lum
1 + "1"             # error de compilación
"a" - "b"           # error de compilación: '-' no aplica a string
[1, 2] + [3]        # [1, 2, 3] — concatenación, válida
```

## 36. Listas y diccionarios literales

```lum
List<int> xs = [1, 2, 3]
List<int> vacia = []

Dict<string,int> d = { "uno": 1, "dos": 2 }
Dict<string,int> vacio = {}
```

Un literal de diccionario nunca se confunde con un bloque de sentencias: los bloques se
marcan por indentación (§1), así que `{` **siempre** abre un diccionario, en cualquier
posición donde valga una expresión. No hace falta desambiguar por contexto como en
JavaScript, donde `{}` al principio de una sentencia es un bloque vacío y en cualquier otra
posición es un objeto vacío.

Toda clave de un `dict_literal` se evalúa en tiempo de ejecución y debe dar un `string`; ver
§8 para el error exacto que produce lo contrario.

## 37. Indexación

```lum
List<int> l = [10, 20, 30]
l[0] = 99                    # asignación indexada
l[2] = l[1] + 5              # el lado derecho puede leer del mismo contenedor
l[0] += 10                   # compuesta, sobre un índice

Dict<string,int> d = { "a": 1 }
d["b"] = 2
d["a"] = d["a"] + 10
```

**El índice de una `List` tiene que ser `int`**, y estar dentro de rango — fuera de rango es
un error en tiempo de ejecución, no un `null` silencioso:

```
{"error":"indice fuera de rango: 5 (tamano 1)"}
```

**Una clave de `Dict` que no existe, en cambio, da `null`** al leer con `[ ]` — no lanza
error. Es una asimetría deliberada entre los dos contenedores: una lista tiene un tamaño
conocido y salirse de él casi siempre es un bug del handler que conviene que aparezca fuerte;
un diccionario se usa a menudo como un mapa disperso donde "la clave no está" es un caso
normal del negocio, no un error de programación.

```lum
Dict<string,int> d = { "a": 1 }
d["z"]          # null, no un error
List<int> l = [1]
l[5]            # {"error": "indice fuera de rango: 5 (tamano 1)"}
```

## 38. Llamadas, argumentos posicionales y nombrados

```
args ::= arg { "," arg }
arg  ::= expr | IDENT "=" expr
```

```lum
render("pagina.html", titulo="T", nota="N")
```

Los argumentos nombrados van **después** de los posicionales, nunca antes ni intercalados —
`f(a=1, b)` no es válido, `f(b, a=1)` sí. Un parámetro con valor por defecto que no se nombra
explícitamente se rellena en orden posicional como cualquier otro.

## 39. Encadenado postfijo

```
postfix ::= primary { "." IDENT | "(" [ args ] ")" | "[" expr "]" | "++" | "--" }
```

Todo lo que sigue a un valor —acceso a campo, llamada, índice, incremento— se encadena en la
misma posición sintáctica, de izquierda a derecha, sin límite de profundidad:

```lum
pedidos[0].cliente.direccion.ciudad.upper()
render("x.html").status(203).header("X-Cache", "miss")
```

Y **cada paso de la cadena se comprueba al compilar** si el tipo del receptor se conoce en
ese punto — ver Parte VI.

## 40. `await` y asincronía

```lum
get endpoint("/lento/:ms", int ms):
    await sleep(ms)
    return { "esperado": ms }
```

`await` solo es válido dentro de un handler de ruta, y solo sobre una expresión que el
checker sepa que es *suspendible*. En 2.0 son suspendibles: `sleep(ms)`, `ws.recv()`, y todos
los métodos de los módulos de base de datos (`query`, `exec`, `begin`, `commit`, `rollback`,
`last_id` — §46).

Tres reglas comprobadas **al compilar**, no en producción:

```lum
sleep(100)             # ERROR: 'sleep()' es asincrono: hay que escribir 'await sleep(...)'
await text("x")        # ERROR: 'text()' no es asincrono: sobra el 'await'
await 5                # ERROR: 'await' solo se aplica a una llamada asincrona
```

`await` suspende el handler completo y devuelve el control al *event loop* de ese hilo; el
resto de conexiones atendidas por el mismo hilo siguen avanzando mientras tanto. Ocho
peticiones concurrentes que hacen `await sleep(500)` tardan 500 ms en total, no cuatro
segundos — es la diferencia entera entre un servidor asíncrono y uno que bloquea un hilo por
petición.

`sleep()` se despierta antes de tiempo si el cliente se desconecta; en ese caso el handler no
continúa ejecutándose después del `await`.

Por dentro, esto es lo que obliga a que el VM tenga pila y locales propios en vez de usar la
pila de llamadas de C++: un intérprete que recursa sobre la pila nativa no puede pararse a
mitad y devolver el control a otro sitio. Cada petición en vuelo vive en su propio marco de
VM, alojado dentro de la corrutina del handler.

---

# Parte VI — Comprobado al compilar, no al ejecutar

## 41. Métodos, campos y tipos se resuelven al compilar

Cuando el tipo del receptor se conoce en el punto de la llamada —un parámetro con tipo
declarado, una variable declarada con tipo, un literal— el nombre del método y el número de
argumentos se comprueban **ahí**, no la primera vez que ese código se ejecuta:

```lum
get endpoint("/a", string quien):
    return { "r": quien.mayusculas() }
```

```
./app.lum:2:19: error: los valores de tipo string no tienen el metodo 'mayusculas';
                  tienen status, header, cookie, starts_with, ends_with, contains,
                  upper, lower, trim
```

La comprobación sigue **por toda la cadena**, porque cada método sabe qué tipo devuelve:
`s.upper().recortar()` también falla al compilar, en la posición exacta de `.recortar()`. Lo
mismo pasa con los campos de una clase:

```lum
class Punto:
    int x
    int y

get endpoint("/z"):
    Punto p = Punto(1, 2)
    return { "z": p.z }
```

```
./app.lum:7:16: error: 'Punto' no tiene un campo 'z'; tiene x, y
```

**Esto llega también dentro de las plantillas**, porque `render()` le pasa al motor de
plantillas los tipos de los argumentos con los que se la llamó, y el motor de plantillas los
comprueba con el mismo checker que el resto del lenguaje: `{{ quien.mayusculas() }}` es un
error de `lumen --check` con el fichero y la línea **de la plantilla**, no una excepción a
media renderización con la mitad de la página ya enviada al cliente.

Donde el tipo del receptor **no** se conoce estáticamente —una variable de un `for` sobre un
`Json`, un campo leído de un `Dict<string,Json>`— no hay nada que comprobar y el despacho
sigue siendo en tiempo de ejecución, como en cualquier lenguaje dinámico.

Tabla completa de métodos disponibles, por tipo receptor:

| Receptor | Métodos |
|---|---|
| Cualquier valor de retorno | `status(código)` `header(k, v)` `cookie(k, v, ...)` |
| `string` | `starts_with(s)` `ends_with(s)` `contains(s)` `upper()` `lower()` `trim()` |
| `List<T>` | `add(v)` |
| `Dict<K,V>` | `has(clave)` `keys()` |
| `File` | `save(directorio)` |

## 42. Inferencia bidireccional

El tipo esperado del lado izquierdo de una asignación **baja** hacia el literal del lado
derecho:

```lum
List<string> xs = []
```

El `[]` de la derecha toma su tipo elemento (`string`) del `List<string>` de la izquierda.
Sin esto habría que escribir algo como `List<string> xs = List<string>()`, que es
exactamente la ceremonia que Lumen Script intenta evitar en cada decisión de diseño del lenguaje —
ver [README.md](README.md#decisiones-de-diseño) para el resto de decisiones y sus motivos.

---

# Parte VII — Retorno de un handler

## 43. Formas de `return`

Todo lo que sale de un handler sale por `return`. No existe un objeto `response` mutable que
haya que ir rellenando y arrastrar por el cuerpo de la función.

| Forma | Resultado |
|---|---|
| `return <instancia-de-clase>` | `200`, cuerpo serializado a JSON |
| `return { ... }` / `return [ ... ]` | `200`, cuerpo JSON |
| `return render("x.html", k=v, ...)` | `200`, HTML renderizado con el motor de plantillas de Lumen Script |
| `return text("...")` | `200`, `text/plain` |
| `return html("...")` | `200`, `text/html` |
| `return send_file(ruta)` | El fichero, servido con `sendfile(2)` — sin copiar por espacio de usuario |
| `return redirect(ruta)` | `302` |
| `return redirect(ruta, código)` | El código dado, típicamente `301` |
| `return status(código)` | Código sin cuerpo |
| `return` (sin expresión) | `204` |

```lum
get endpoint("/descarga"):
    return send_file("/var/files/informe.pdf")

get endpoint("/legacy"):
    return redirect("/nuevo", 301)
```

## 44. Encadenado: `status`, `header`, `cookie`

Toda forma de `return` admite encadenar estas tres llamadas, en cualquier orden y cualquier
combinación:

```lum
return { "id": 1 }.status(201)
return { "a": 1 }.header("X-Cosa", "valor")
return { "n": 1 }.status(202).header("X-Uno", "1").header("X-Dos", "2")
return render("x.html").status(203)

return { "ok": true }.cookie("tema", "oscuro",
                             max_age=3600, http_only=false, same_site="strict")
```

Opciones de `cookie`: `max_age`, `path`, `domain`, `secure`, `http_only`, `same_site`
(`"lax"` | `"strict"` | `"none"`). Por defecto: `path=/`, `HttpOnly` activo, `SameSite=Lax`.

---

# Parte VIII — Objetos reservados

## 45. La superficie completa

| Objeto | Miembros | Dónde |
|---|---|---|
| `request` | `path` `method` `ip` | Cualquier handler |
| `session` | cualquier campo (almacén libre), `clear()` | Cualquier handler, con `session:` configurado en `app:` |
| `jwt` | `valid` `claims` | Cualquier handler, bajo una guarda `jwt` |
| `state` | `incr(k[,n])` `decr(k[,n])` `get(k[,def])` `set(k,v)` `remove(k)` | Cualquier handler y función |
| `log` | `info(msg)` `warn(msg)` `error(msg)` | En todas partes |
| `sse` | `send(datos)` `send(evento,datos)` `send(evento,datos,id)` `ping([texto])` `open` | Rutas `sse` |
| `ws` | `send(msg)` `await recv()` `open` `close()` | Rutas `ws` |
| `error` | `code` `message` `messages` | Bloques `on error` |
| `this` | los campos y métodos de la clase | Métodos y constructores |

`session` es un almacén de campos libres, no una clase con forma fija — `session.<lo-que-sea>`
admite cualquier nombre, y leer uno que nunca se escribió da `null`:

```lum
post endpoint("/login", Login datos):
    session.usuario = datos.nombre
    session.rol     = datos.nombre == "alice" ? "admin" : "user"
    return { "ok": true }

get endpoint("/quien"):
    return { "usuario": session.usuario, "rol": session.rol }
```

Va en una cookie firmada con HMAC-SHA256 —firmada, **no cifrada**: el cliente puede leer el
contenido, solo no puede falsificarlo—, así que nada que el usuario no deba ver va ahí. Una
firma inválida deja la sesión vacía entera, nunca a medias.

`state` es la única vía de estado compartido entre los distintos hilos de *event loop*: cada
uno tiene su propio VM con su propia pila y su propio heap, y no comparten memoria entre sí.
Por eso `state` expone **operaciones** (`incr`, `decr`) en vez de una propiedad que se pudiera
leer y reescribir a mano — escribir `state.x = state.x + 1` sería una carrera de lectura
seguida de escritura entre dos hilos, mientras que `state.incr("x")` es una única operación
atómica que además devuelve el valor ya actualizado. Vive en memoria del proceso: se pierde al
reiniciar y no se comparte entre máquinas distintas.

`jwt.claims` y el resultado de una consulta a base de datos son valores `Json`: se leen con
`[ ]`, nunca con `.`, porque un `Json` no tiene campos fijos que el checker pueda verificar:

```lum
get endpoint("/yo"):
    return { "sub": jwt.claims["sub"], "rol": jwt.claims["rol"] }
```

## 46. Los módulos de base de datos

`sqlite`, `postgres` y `mysql` se comportan como objetos reservados una vez importados y
configurados (§13, §14): `query`, `exec`, `begin`, `commit`, `rollback`, y `last_id` —este
último solo en `sqlite` y `mysql`, porque postgres no tiene un equivalente fiable y el módulo
lo dice en vez de inventárselo—. Todos sus métodos son asíncronos: se llaman siempre con
`await` (§40).

```lum
get endpoint("/articulos/:id", int id):
    List<Json> filas = await sqlite.query(
        "select titulo from articulos where id = ?", id)
    if len(filas) == 0:
        return status(404)
    return filas[0]
```

`query()` devuelve `List<Json>`. `exec()` devuelve el número de filas afectadas, como `int`.
Un error del motor —una tabla que no existe, una violación de una restricción— **no revienta
el handler**: llega como un valor `Json` con la clave `error`, para que el handler decida qué
hacer con él en vez de que el fallo salte directamente a un `500` genérico:

```lum
get endpoint("/malo"):
    Json r = await sqlite.query("select * from tabla_que_no_existe")
    return r          # { "error": "no such table: tabla_que_no_existe" }
```

**Los parámetros van siempre aparte del texto de la consulta, nunca concatenados** — es la
única forma de abrirse a una inyección SQL, y el lenguaje no da ninguna vía cómoda para
hacerlo mal. El marcador es `?` en los tres motores: aunque postgres numera los suyos
internamente (`$1`, `$2`...), el propio driver traduce la consulta antes de mandarla, así que
la misma cadena vale sin tocar una letra en `sqlite`, `mysql` y `postgres`:

```lum
await sqlite.query(  "select titulo from articulos where id = ?", id)
await mysql.query(   "select titulo from articulos where id = ?", id)
await postgres.query("select titulo from articulos where id = ?", id)
```

Una consulta escrita ya con `$1` sale intacta hacia postgres, así que código anterior a esta
traducción sigue funcionando sin cambios. Un `?` dentro de una cadena, un identificador
entrecomillado, un comentario, o un bloque `$$...$$` no se toca. Mezclar `?` y `$1` en la
misma consulta es un error, porque la numeración chocaría.

Transacciones: `begin()` fija la conexión del pool para el resto de la petición, y
`commit()`/`rollback()` la sueltan. Si el handler termina —o revienta— con una transacción
abierta, Lumen hace `ROLLBACK` por su cuenta y lo avisa por consola; sin eso, la siguiente
petición que reutilizara esa conexión del pool heredaría una transacción a medias que no es
suya.

```lum
post endpoint("/transfiere"):
    await sqlite.begin()
    await sqlite.exec("update cuentas set saldo = saldo - 30 where nombre = ?", "ana")
    await sqlite.exec("update cuentas set saldo = saldo + 30 where nombre = ?", "bob")
    await sqlite.commit()
    return { "ok": true }
```

---

# Parte IX — Ambigüedades y decisiones del parser

## 47. Tabla de ambigüedades

| Caso | Resolución |
|---|---|
| `>>` al cerrar genéricos anidados | El lexer nunca fusiona `>` `>` en un solo token (§6) |
| `?` de tipo opcional frente a ternario | Un token de *lookahead*: tras `?`, un `IDENT` es declaración (§11) |
| `{` de bloque de sentencias frente a literal de diccionario | Los bloques son siempre por indentación; `{` en posición de expresión siempre abre un diccionario (§36) |
| `else` colgante | Imposible: los bloques son por indentación, no hay `{ }` que desambiguar (§23) |
| `elif` frente a `else if` | Las dos formas son válidas y equivalentes (§23) |

---

# 48. Tres programas completos

## Login con sesión y zona protegida por rol

```lum
app:
    port      8090
    templates "./templates"

    session:
        secret  env("SESSION_SECRET")
        max_age 3600
        secure  false          # en local, sin TLS por delante

    jwt:
        secret env("JWT_SECRET")
        issuer "mi-app"


class Login:
    string nombre
    string contrasena

    validate:
        nombre != ""      "nombre: obligatorio"
        contrasena != ""  "contrasena: obligatoria"


post endpoint("/login", Login datos):
    if datos.contrasena != "hunter2":
        return status(401)

    session.usuario = datos.nombre
    session.rol     = datos.nombre == "alice" ? "admin" : "user"
    return { "ok": true, "rol": session.rol }

get endpoint("/quien"):
    return { "usuario": session.usuario, "rol": session.rol }

post endpoint("/logout"):
    session.clear()
    return { "ok": true }


group("/admin"):
    require session.rol == "admin" else status(403)

    get endpoint("/panel"):
        return { "panel": true, "de": session.usuario }

    # Los grupos anidan y las guardas se acumulan: para llegar aqui hay que
    # pasar primero la del padre.
    group("/peligro"):
        require session.usuario == "alice" else status(403)

        get endpoint("/boton"):
            return { "pulsado": true }


group("/api"):
    require jwt.valid else status(401)

    get endpoint("/yo"):
        return { "sub": jwt.claims["sub"], "rol": jwt.claims["rol"] }
```

## CRUD con transacciones sobre sqlite

```lum
import sqlite

app:
    port 8070
    sqlite:
        file "./cuentas.db"
        pool 4

class Cuenta:
    string nombre
    int    saldo

    validate:
        nombre != ""   "nombre: obligatorio"
        saldo >= 0     "saldo: no puede ser negativo"

get endpoint("/saldos"):
    return await sqlite.query("select nombre, saldo from cuentas order by nombre")

post endpoint("/cuentas", Cuenta c):
    int filas = await sqlite.exec(
        "insert into cuentas (nombre, saldo) values (?, ?)", c.nombre, c.saldo)
    int id = await sqlite.last_id()
    return { "id": id, "filas": filas }.status(201)

# Transferencia atomica: las dos actualizaciones o ninguna.
post endpoint("/transfiere/:cuanto", int cuanto):
    await sqlite.begin()
    await sqlite.exec("update cuentas set saldo = saldo - ? where nombre = 'ana'", cuanto)
    await sqlite.exec("update cuentas set saldo = saldo + ? where nombre = 'bob'", cuanto)
    await sqlite.commit()
    return { "movido": cuanto }

# Se deshace a mano si algo no cuadra.
post endpoint("/deshecha/:cuanto", int cuanto):
    await sqlite.begin()
    await sqlite.exec("update cuentas set saldo = saldo - ? where nombre = 'ana'", cuanto)
    await sqlite.rollback()
    return { "deshecho": true }

on error 422:
    return { "detalles": error.messages }

on error:
    log.error(error.message)
    return { "error": "algo se rompio", "en": request.path }
```

## Tiempo real: SSE y WebSocket sobre estado compartido

```lum
app:
    port 8087

get endpoint("/contador"):
    return { "count": state.get("contador", 0) }

post endpoint("/contador/incr"):
    return { "count": state.incr("contador") }


sse endpoint("/contador/live"):
    sse.send("snapshot", { "count": state.get("contador", 0) })

    int tick = 0
    while sse.open:
        await sleep(2000)
        tick = tick + 1
        sse.send("delta", { "count": state.get("contador", 0), "tick": tick })
        if tick % 10 == 0:
            sse.ping("keepalive")


ws endpoint("/contador/ws") origins("https://miapp.com", "http://localhost:5173"):
    ws.send({ "count": state.get("contador", 0) })

    while ws.open:
        Json msg = await ws.recv()
        if msg == null:                 # el cliente cerro la conexion
            break

        string accion = msg["accion"]
        if accion == "incrementa":
            ws.send({ "count": state.incr("contador") })
        elif accion == "decrementa":
            ws.send({ "count": state.decr("contador") })
        elif accion == "resetea":
            state.set("contador", 0)
            ws.send({ "count": 0 })
```

---

# Apéndice A — Gramática formal completa

```
program        ::= { top_level }
top_level      ::= import_decl | class_decl | fn_decl | app_decl
                 | group_decl  | route_decl | error_decl

import_decl    ::= "import" IDENT NEWLINE

class_decl     ::= "class" IDENT ":" INDENT { class_member } DEDENT
class_member   ::= field | validate_block | constructor | method
field          ::= type IDENT NEWLINE
validate_block ::= "validate" ":" INDENT { validate_rule } DEDENT
validate_rule  ::= expr STRING NEWLINE
constructor    ::= IDENT "(" [ params ] ")" ( ":" block | NEWLINE )
method         ::= "fn" type IDENT "(" [ params ] ")" ":" block

fn_decl        ::= "fn" type IDENT "(" [ params ] ")" ":" block
params         ::= param { "," param }
param          ::= type IDENT [ "=" expr ]

app_decl       ::= "app" ":" INDENT { app_entry } DEDENT
app_entry      ::= "name"      STRING
                 | "version"   STRING
                 | "port"      INT
                 | "templates" STRING
                 | "static"    STRING "->" STRING [ "spa" ]
                 | "docs" | "health" | "metrics"
                 | IDENT ":" INDENT { config_pair } DEDENT
config_pair    ::= IDENT expr NEWLINE

route_decl     ::= method "endpoint" "(" STRING { "," param } ")" [ modifier ] ":" block
method         ::= "get" | "post" | "put" | "patch" | "delete" | "any" | "sse" | "ws"
modifier       ::= "origins" "(" STRING { "," STRING } ")"

group_decl     ::= "group" "(" STRING ")" ":" INDENT { group_member } DEDENT
group_member   ::= require_stmt | route_decl | group_decl

error_decl     ::= "on" "error" [ INT ] ":" block

block          ::= INDENT { statement } DEDENT
statement      ::= var_decl | assign_stmt | if_stmt | while_stmt | for_stmt
                 | return_stmt | require_stmt | try_stmt
                 | "break" NEWLINE | "continue" NEWLINE | expr NEWLINE

var_decl       ::= type IDENT [ "=" expr ] NEWLINE
assign_stmt    ::= lvalue ( "=" | "+=" | "-=" | "*=" | "/=" | "%=" ) expr NEWLINE
lvalue         ::= IDENT { "." IDENT | "[" expr "]" }

if_stmt        ::= "if" expr ":" block
                    { ( "elif" | "else" "if" ) expr ":" block }
                    [ "else" ":" block ]
while_stmt     ::= "while" expr ":" block
for_stmt       ::= "for" type IDENT "in" expr ":" block
return_stmt    ::= "return" [ expr ] NEWLINE
require_stmt   ::= "require" expr "else" expr NEWLINE
try_stmt       ::= "try" ":" block "catch" [ IDENT ] ":" block

expr           ::= ternary
ternary        ::= or_expr [ "?" expr ":" expr ]
or_expr        ::= and_expr { "or" and_expr }
and_expr       ::= not_expr { "and" not_expr }
not_expr       ::= [ "not" ] equality
equality       ::= comparison { ( "==" | "!=" ) comparison }
comparison     ::= sum { ( "<" | "<=" | ">" | ">=" ) sum }
sum            ::= product { ( "+" | "-" ) product }
product        ::= unary { ( "*" | "/" | "%" ) unary }
unary          ::= [ "-" | "++" | "--" ] postfix
postfix        ::= primary { "." IDENT | "(" [ args ] ")" | "[" expr "]"
                            | "++" | "--" }

primary        ::= INT | FLOAT | STRING | BOOL | NULL
                 | IDENT | "this" | "await" expr
                 | dict_literal | list_literal
                 | "(" expr ")"

args           ::= arg { "," arg }
arg            ::= expr | IDENT "=" expr
dict_literal   ::= "{" [ dict_entry { "," dict_entry } ] "}"
dict_entry     ::= expr ":" expr
list_literal   ::= "[" [ expr { "," expr } ] "]"

type           ::= base_type [ "?" ]
base_type      ::= primitive | generic_type | IDENT
primitive      ::= "int" | "long" | "float" | "double" | "bool" | "string" | "void"
generic_type   ::= IDENT "<" type { "," type } ">"

INT            ::= dígito { dígito }
FLOAT          ::= dígito { dígito } "." dígito { dígito }
STRING         ::= '"' { carácter } '"' | '"""' { carácter | salto } '"""'
BOOL           ::= "true" | "false"
NULL           ::= "null"
IDENT          ::= ( letra | "_" ) { letra | dígito | "_" }
```

---

# Apéndice B — Palabras reservadas

```
import  class  fn  app  group  endpoint  on  error  origins
get  post  put  patch  delete  any  sse  ws
if  else  elif  while  for  in  return  require  try  catch  break  continue
validate  and  or  not  true  false  null  this  void  spa
```

Objetos reservados (no palabras clave; existen solo dentro de su contexto — ver §2, §45):
`request` `session` `state` `jwt` `sse` `ws` `error` `log` `this`.

---

# Apéndice C — Mensajes de error citados en este libro

| Mensaje | Sección | Qué falta |
|---|---|---|
| `el patron declara ':id' pero ningun parametro lo recoge` | §17 | Falta el parámetro en la firma |
| `'sleep()' es asincrono: hay que escribir 'await sleep(...)'` | §40 | Falta el `await` |
| `'text()' no es asincrono: sobra el 'await'` | §40 | Sobra el `await` |
| `'await' solo se aplica a una llamada asincrona` | §40 | `await` sobre algo que no suspende |
| `'sse' solo existe dentro de una ruta sse` | §2 | Objeto reservado fuera de contexto |
| `una ruta ws necesita origins(...)` | §17 | Falta la lista blanca de orígenes |
| `no se puede sumar int y string` | §35 | Operación aritmética entre tipos incompatibles |
| `la sesion no esta configurada` | §45 | Falta `session: secret ...` en `app:` |
| `'X' no esta declarada` | §15 | Nombre desconocido, también dentro de `validate` |
| `los valores de tipo string no tienen el metodo 'M'; tienen ...` | §41 | Método inexistente sobre un tipo conocido |
| `'Clase' no tiene un campo 'X'; tiene ...` | §41 | Campo inexistente sobre una clase conocida |
| `la clave de un Dict tiene que ser string` | §8, §36 | Un literal, lectura o escritura de `Dict` con clave no textual |
| `indice fuera de rango: N (tamano M)` | §37 | Acceso a `List` fuera de sus límites |
| `el binario no se compilo con soporte de 'X'` | §13 | `import` de un módulo de base de datos no enlazado |

Todos los errores de compilación salen con fichero, línea, columna, y un cursor bajo la
posición exacta — no hay un mensaje genérico "hay un error en algún sitio de tu programa".
