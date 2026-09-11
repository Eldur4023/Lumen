# Lumen Script — The language

> The complete reference manual: lexis, types, grammar and semantics of Lumen Script, the
> language Lumen compiles and serves. It replaces the `ejemplo-*.lum` and `prueba-*.lum` files that predate it
> that used to live in the repository root — what they taught by demonstration, this document
> teaches explained, with the same grammar but zero ambiguity about what is part of the
> language and what was just a whim of whoever wrote the example.
>
> Notation of the formal grammar: `::=` a production, `|` an alternative, `[ ]` optional,
> `{ }` zero or more repetitions, `( )` grouping, UPPERCASE a terminal the lexer produces,
> `"text"` an exact literal. Every unmarked code block is valid Lumen Script that compiles as
> is — nothing that follows is pseudocode.
>
> For the practical guide on putting an application together —project layout, how it starts,
> what the binary tests about itself— see [GUIDE.md](GUIDE.md). For the design decisions and
> their reasons, [README.md](README.md#design-decisions). This document is the third one: the
> language itself, top to bottom, taking nothing for granted.

---

## Contents

**Part I — Lexis**
[1](#1-comments-and-blocks) · [2](#2-keywords-and-reserved-objects) · [3](#3-identifiers)
· [4](#4-literals) · [5](#5-strings) · [6](#6-the--trap)

**Part II — Types**
[7](#7-primitives) · [8](#8-native-classes) · [9](#9-optionality) · [10](#10-generics)
· [11](#11-type--versus-the-ternary)

**Part III — Structure of a program**
[12](#12-files-and-two-pass-compilation) · [13](#13-import) · [14](#14-app)
· [15](#15-class) · [16](#16-fn) · [17](#17-routes) · [18](#18-route-parameters)
· [19](#19-group) · [20](#20-on-error)

**Part IV — Statements**
[21](#21-variable-declaration) · [22](#22-assignment) · [23](#23-if--elif--else-if--else)
· [24](#24-while) · [25](#25-for) · [26](#26-return) · [27](#27-require--else)
· [28](#28-trycatch) · [29](#29-break-and-continue) · [30](#30--and---as-expressions)

**Part V — Expressions**
[31](#31-full-precedence) · [32](#32-ternary) · [33](#33-booleans-and-truthiness)
· [34](#34-equality-and-comparison) · [35](#35-arithmetic-without-coercion)
· [36](#36-list-and-dictionary-literals) · [37](#37-indexing)
· [38](#38-calls-positional-and-named-arguments) · [39](#39-postfix-chaining)
· [40](#40-await-and-async)

**Part VI — Checked at compile time, not at run time**
[41](#41-methods-fields-and-types-are-resolved-at-compile-time) · [42](#42-bidirectional-inference)

**Part VII — Returning from a handler**
[43](#43-forms-of-return) · [44](#44-chaining-status-header-cookie)

**Part VIII — Reserved objects**
[45](#45-the-complete-surface) · [46](#46-the-database-modules)

**Part IX — Ambiguities and parser decisions**
[47](#47-table-of-ambiguities)

**Complete programs**
[48](#48-three-complete-programs)

**Appendices**
[A](#appendix-a--the-complete-formal-grammar) · [B](#appendix-b--reserved-words)
· [C](#appendix-c--error-messages-quoted-in-this-book)

---

# Part I — Lexis

## 1. Comments and blocks

A comment starts at `#` and runs to the end of the line. There are no block comments — if you
needed one for a long string, that string should probably be a triple-quoted string (§5).

```lum
# this is a comment
int x = 1   # and so is this
```

Blocks are marked by indentation, as in Python: the lexer emits `INDENT` and `DEDENT` tokens
by comparing each line's indentation with the previous one's, and a block is the sequence of
statements indented more than the line that opens it.

```lum
if x > 0:
    log.info("positive")
    if x > 100:
        log.info("and large")
    log.info("end of the first level")
log.info("outside the if")
```

**Spaces only.** A line starting with a tab is not translated into an equivalent number of
columns — that would mean deciding how much a tab is worth, and any value you pick is a
guess. The lexer treats spaces literally.

There is no `;` to end a statement and no `{ }` for blocks. `{` and `}` are free for one use:
the dictionary literal (§36).

---

## 2. Keywords and reserved objects

Language keywords — they cannot be used as identifiers:

```
import  class  fn  app  group  endpoint  on  error  origins
get  post  put  patch  delete  any  sse  ws
if  else  elif  while  for  in  return  require  try  catch  break  continue
validate  and  or  not  true  false  null  this  void  spa
```

Separate from those are the **reserved objects**: they are not declared, they exist only
inside the context that defines them, and using one out of place is a compile error, not a
`null` reference in production.

| Object | Available in |
|---|---|
| `request` | any handler |
| `session` | any handler |
| `state` | any handler and function |
| `jwt` | handlers under a `jwt` guard |
| `sse` | `sse` handlers |
| `ws` | `ws` handlers |
| `error` | `on error` blocks (`code`, `message`, `messages`) |
| `log` | everywhere |
| `this` | methods and constructors |

```
./app.lum:12:9: error: 'sse' only exists inside an sse route
```

---

## 3. Identifiers

```
IDENT ::= ( letter | "_" ) { letter | digit | "_" }
```

`letter` includes Unicode: `contraseña`, `título`, `año` are valid identifiers — the lexer
does not force you to transliterate names into ASCII to write them in your own language.

```lum
class User:
    string  name
    string? contraseña
    int     año_de_alta
```

---

## 4. Literals

```
INT      ::= digit { digit }
FLOAT    ::= digit { digit } "." digit { digit }
BOOL     ::= "true" | "false"
NULL     ::= "null"
```

There is no scientific notation (`1.0e308` does not lex) and no thousands separators
(`1_000_000` does not lex). An integer does not fit an implicit decimal point: `1.` and `.5`
are not valid literals, a digit is needed on each side of the dot.

`true`, `false` and `null` are lowercase — this is Lumen Script, not C++, and certainly not
`prueba.lum`, the language's first sketch, which used Python-style capitalized `True`/`False`
and never compiled against the real Lumen Script.

---

## 5. Strings

```
STRING ::= '"' { character } '"' | '"""' { character | line break } '"""'
```

Escapes, the same in both forms: `\n` `\t` `\r` `\0` `\"` `\\`.

```lum
string greeting = "hello\tworld\n"
```

### Triple-quoted strings

For SQL or HTML, where fighting `\n` on every line would be pure noise:

```lum
string q = """
    SELECT id, title
    FROM posts
    WHERE author = ?
    """
```

The margin is **not** part of the string — it is the source file's indentation, not the
text's. It is stripped with three rules:

1. A line break right after the opening quote does not count.
2. If the closing quote stands alone on its line, that line does not count either.
3. From the rest, the indentation **common** to every non-empty line is removed — which keeps
   the *relative* indentation between them.

The example above is exactly `SELECT id, title\nFROM posts\nWHERE author = ?`, with no line
break at the start or the end. With relative indentation:

```lum
string html = """
    <ul>
      <li>one</li>
      <li>two</li>
    </ul>
    """
```

is `<ul>\n  <li>one</li>\n  <li>two</li>\n</ul>` — the `<ul>` line loses its common 4-space
margin and the `<li>` lines keep the 2 extra spaces they had relative to it.

Only spaces are considered: a line starting with a tab inside the string leaves the computed
margin at zero and nothing is trimmed from that string, instead of guessing how much a tab is
worth and risking getting it wrong.

---

## 6. The `>>` trap

The lexer **does not merge** two consecutive `>` into a right-shift token, so a nested
generic closes without a separating space:

```lum
Dict<string, List<int>> by_category
```

C++ carried this bug until C++11, where `vector<vector<int>>` was a syntax error and you had
to write `vector<vector<int> >` with a space. Lumen Script avoids it from day one simply by
never creating the `>>` token — the lexer never combines two operators into one without a
real lexical reason to do so.

---

# Part II — Types

## 7. Primitives

```
primitive ::= "int" | "long" | "float" | "double" | "bool" | "string" | "void"
```

Lowercase, always. `void` is only valid as the return type of a `fn` that returns nothing
(§16).

**A detail documented nowhere else: `int` and `long` are the same type inside.** The value is
kept in a signed 64-bit integer whether the field was declared `int` or `long`; the same goes
for `float` and `double`, which share a double-precision floating point representation. All
four words exist because code ported from another language usually arrives written with one
of the two, and forcing a rewrite would be ceremony with no benefit — but as far as range and
precision go, choosing `int` over `long`, or `float` over `double`, changes nothing.

```lum
int    a = 9223372036854775807   # it fits, it is a long long inside
long   b = 5                     # exactly the same type as 'a'
float  c = 3.14159265358979      # double precision, even though it is called float
double d = 3.14159265358979      # identical to 'c'
```

## 8. Native classes

| Type | What it is |
|---|---|
| `Json` | Dynamic data: a schemaless body, a WebSocket message, a response literal |
| `List<T>` | Homogeneous list |
| `Dict<K,V>` | Homogeneous map |
| `File` | Uploaded file: `name`, `filename`, `content_type`, `size`, and the method `save(dir)` |

```
generic_type ::= IDENT "<" type { "," type } ">"
```

In practice, the only `Dict` key that exists is `string` — and not by convention: the VM
checks it in three places (building the literal, reading with `[ ]`, writing with `[ ]`) and
anything else is a runtime error:

```
{"error":"a Dict key must be a string, not int"}
```

Generics are **erased**: the checker verifies `List<T>` and `Dict<K,V>` at compile time and
the type disappears before bytecode is emitted. The VM never sees a `List<File>` at run time,
only a list — the same technique as Java, and for the same reason: generics exist so the
compiler can watch, not so the interpreter carries information it has already used.

**There are no user-defined generic classes.** `class Box<T>:` is not a valid declaration. It
is a deliberate limitation, not an oversight, and it is additive: it can arrive in a later
version without breaking anything that compiles today.

## 9. Optionality

```
type ::= base_type [ "?" ]
```

`T?` marks that the value may be missing.

```lum
class User:
    string  name
    string? nickname       # may not arrive
```

In a class that receives a request body (§18), a field that is **not** optional and is
missing from the JSON automatically produces a `422`, without the handler ever running — the
checker knows which fields are required and the runtime demands them before building the
object. An optional one that is missing simply stays `null`:

```
POST /u  {"nickname":"x"}          -> 422 {"messages":["name: required"]}
POST /u  {"name":"ana"}            -> 200 {"name":"ana","nickname":null}
```

**The trap inherited from Python:** with an optional value, `if nickname:` does not tell
"it arrived and is an empty string" from "it did not arrive". To ask about presence you have
to compare against `null` explicitly:

```lum
if nickname == null:
    nickname = name
```

## 10. Generics

Covered in practice in §8. The formal rule:

```
base_type ::= primitive | generic_type | IDENT
```

An `IDENT` in type position is the name of a `class` declared in any file of the project —
the order between files does not matter (§12).

## 11. Type `?` versus the ternary

`?` appears in two places in the grammar with different meanings, and both are valid in the
same syntactic position:

```lum
string?  x = a ? b : c
```

Here the first `?` marks `string` as optional; the second opens a ternary. It is not
ambiguous, but it requires the parser to look one token ahead: after a `?` in type position
there is always an `IDENT` (the name of the variable being declared); in a ternary, after the
`?` comes a complete expression followed by `:`. A single token of difference is enough to
tell the two cases apart without backtracking.

---

# Part III — Structure of a program

## 12. Files and two-pass compilation

```
program   ::= { top_level }
top_level ::= import_decl | class_decl | fn_decl | app_decl | group_decl
            | route_decl  | error_decl
```

The order between declarations **and between files** does not matter. Compiling a whole
project happens in two passes: first every file is walked collecting what exists —classes,
functions, routes— and only then are the names inside each body resolved. That is why a class
can be used before it is declared, and a function can call another that appears further down
in the same file or in a different one (see §16, `accumulate`/`sum_up_to` calling each other
in reverse declaration order).

## 13. `import`

```
import_decl ::= "import" IDENT NEWLINE
```

It introduces a namespace. The available modules are `sqlite`, `postgres` and `mysql` —the
three persistence modules— and each one also needs its own configuration block inside `app:`
(§14, §46).

```lum
import postgres

app:
    postgres:
        host     "127.0.0.1"
        database "my_app"
        user     "lumen_script"
        password env("PG_PASSWORD")
```

Each module is compiled into the Lumen binary only if its native client was present when
Lumen itself was built. Importing one the binary does not carry is an error **when compiling
the `.lum`**, not a confusing failure the first time it is called:

```
./app.lum:1:8: error: module 'postgres' is not compiled into this binary
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

It can be in any file of the project, but **only once** — there is no way to split the
configuration between two files, on purpose: whoever reads the project for the first time
knows there is a single place to look.

```lum
app:
    name      "My application"
    version   "1.0.0"
    port      8080
    templates "./templates"

    static "/static" -> "./public"
    static "/"       -> "./dist" spa

    docs                      # enables /openapi.json and /docs
    health                    # enables /health
    metrics                   # enables /metrics, Prometheus format

    session:
        secret  env("SESSION_SECRET")
        max_age 86400
        secure  true

    jwt:
        secret env("JWT_SECRET")
        issuer "my-app"

    sqlite:
        file "./data.db"
        pool 8
```

`env("VAR")` is resolved **at compile time**, not when the process starts: it is the only way
a secret does not end up written in the `.lum` that is versioned in git.

`spa` on a static mount makes routes that find no file fall back to `index.html` — what any
client-side router needs so that refreshing the page on `/profile/42` does not give a 404
from the static server.

The generic `IDENT ":" ...` block is what lets `session:`, `jwt:`, `sqlite:`, `postgres:` and
`mysql:` share the same syntactic shape without the grammar having to know each module — each
one interprets its own `config_pair` afterwards.

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

### Fields and validation

```lum
class User:
    int     id
    string  name
    int     age
    string? password            # ? = may be missing

    validate:
        name != ""      "name: required"
        age >= 0        "age: cannot be negative"
        age < 150       "age: hardly believable value"
```

Every `validate_rule` is a boolean expression followed by the message emitted if it is false.
**Every failing message comes out at once**, not just the first — a form with four errors gets
all four in one go, instead of forcing you to fix one, resubmit, discover the next. If any
rule fails, the handler **never runs**.

The `validate` rules are compiled like any other expression, so a misspelled field in a rule
is a compile error and never reaches production:

```
./app.lum:9:9: error: 'namme' is not declared
```

### Constructors

```lum
class Point:
    int x
    int y

    Point(int x):              # with a body
        this.x = x
        this.y = 0

    Point(int x, int y)        # without a body: each parameter goes to its field, in order
```

They are told apart by the **number** of parameters, not by their types — two constructors
with the same arity are a compile error for being ambiguous. A constructor **without a body**
assigns its parameters to the fields of the same name in declaration order; fields that do
not appear among the parameters stay `null` (only valid if they are optional). With no
constructor declared at all, the class gets an implicit one with every field, in order:

```lum
class Box:
    string name
    int    width
    int    height

# ...

Box b = Box("large", 3, 4)   # the implicit constructor
```

### Methods

```lum
class Point:
    int x
    int y

    fn int squared():
        return this.x * this.x + this.y * this.y

    fn string label(string prefix = "P"):
        return prefix + "(" + str(this.x) + "," + str(this.y) + ")"

    fn Point moved(int dx, int dy):
        return Point(this.x + dx, this.y + dy)
```

Methods and constructors compile as normal functions with an implicit `this` first parameter,
so they share the frame stack with `fn` and support recursion and default values in exactly
the same way.

The call `p.squared()` **is resolved at compile time**, from the receiver's declared type —
there is no dynamic dispatch and no method table lookup at run time. A misspelled method
never reaches production:

```
./app.lum:8:20: error: 'Point' has no method 'triple';
                  it has squared, label, moved
```

An instance built by hand (`Point(3, 4)`) and one bound from a request body
(`post endpoint("/p", Point p)`) are exactly the same kind of value: methods work the same on
both.

## 16. `fn`

```
fn_decl ::= "fn" type IDENT "(" [ params ] ")" ":" block
params  ::= param { "," param }
param   ::= type IDENT [ "=" expr ]
```

```lum
fn int double(int x):
    return x * 2

fn string greet(string name, string greeting = "hello"):
    return greeting + ", " + name

fn int accumulate(int n, int acc):
    if n <= 0:
        return acc
    return accumulate(n - 1, acc + n)

fn int sum_up_to(int n):
    return accumulate(n, 0)       # calls a function declared FURTHER DOWN
```

A parameter without a default value cannot come after one that has one, as in almost any
language with default arguments. A function with no explicit `return` returns `null`, and its
declared return type must be `void` in that case.

Recursion works with a cap of 200 nested calls. Going over produces a language error —not a
process `stack overflow`, because the VM has its own stack (§40)— so a recursion bug with no
base case stays a readable `500`, not a binary that crashes:

```lum
fn int endless(int n):
    return endless(n + 1)
```

An error inside a function can be caught where it is called:

```lum
fn int breaks(int n):
    return n / 0

get endpoint("/catch/:n", int n):
    try:
        return { "no": breaks(n) }
    catch e:
        return { "caught": e.message }
```

Declaring a function with the same name as a builtin (`len`, `str`, `render`...) is a compile
error. Functions can also be called inside a `validate:` block:

```lum
fn bool is_email(string s):
    return s.contains("@") and s.contains(".")

class Signup:
    string email

    validate:
        is_email(email)   "email: invalid format"
```

## 17. Routes

```
route_decl ::= method "endpoint" "(" STRING { "," param } ")" [ modifier ] ":" block
method     ::= "get" | "post" | "put" | "patch" | "delete" | "any" | "sse" | "ws"
modifier   ::= "origins" "(" STRING { "," STRING } ")"
```

```lum
get    endpoint("/path"):
post   endpoint("/path"):
put    endpoint("/path"):
patch  endpoint("/path"):
delete endpoint("/path"):
any    endpoint("/path"):                       # any method
sse    endpoint("/path"):                       # event stream, implies GET
ws     endpoint("/path") origins("https://x"):  # WebSocket
```

The first `STRING` is the route pattern: `/users/:id`, `/users/{id}` (both forms are
equivalent) and `/files/*` for a trailing wildcard. **The checker verifies both directions**:
that every `:name` in the pattern has a parameter binding it in the signature, and that no
parameter declared as a route segment is left over with no place in the pattern.

```
./app.lum:4:1: error: the pattern declares ':id' but no parameter binds it
```

`origins(...)` is only syntactically valid on `ws`, and there it is **required** — the grammar
accepts it after any route, but the checker rejects it outside a `ws` and rejects a `ws` that
does not carry it. Browsers do not apply the same-origin policy to the WebSocket *handshake*,
so without an allowlist any page could open the connection from a user's browser and inherit
their session cookies.

### The two route levels

A route whose whole body is resolved at compile time —a single `return` of a constant value,
or of a native call with only literal arguments— becomes a **native action**: an entry in the
route radix tree that does not run a single bytecode step.

```lum
get endpoint("/"):
    return render("index.html")        # declarative: zero bytecode per request
```

The rest run bytecode on the VM (§40). On startup, the binary reports how many routes take
each path:

```
lumen: 3 file(s), 12 route(s) — 5 declarative, 7 with logic
```

A route with inherited group guards is **never** declarative, even if its own body would be:
the native action would not run the group's `require`, and skipping it would be a security
hole shaped like an optimization.

## 18. Route parameters

Everything a handler needs is declared in its signature — there is no `request` object to pull
things out of by hand inside the body, except for what is genuinely outside the schema
(`request.path`, `request.method`, `request.ip`, see §45).

```lum
get endpoint("/users/:id", int id, int page = 1, string q):
    return { "id": id, "page": page, "q": q }
```

| Parameter form | Where it comes from |
|---|---|
| A name that appears in the pattern (`:id`) | Route segment |
| A name that does not appear in the pattern | A *query string* parameter; if the request is `multipart/form-data`, also a text field of the form with that name |
| `= value` | Default value if missing from both places above |
| A type that is a `class` | The request's JSON body, validated before entering the handler |
| `File` / `List<File>` | One or several `multipart/form-data` parts |

The *query string* takes priority if the same name appears in both places at once —a rare
case, but a deterministic one. This is what allows mixing files and text in the same form with
nothing special in the signature:

```lum
post endpoint("/avatar", File image, string title):
    require title != "" else status(422)
    string name = image.save("./uploads")
    return { "name": name, "title": title }
```

A query, route or form value that does not fit its declared type is a **400**, not an uncaught
exception:

```json
{"error":"invalid parameter","expected":"int","param":"id","received":"abc"}
```

With `File` (not `List<File>`), a missing file is a `422`. With `List<File>`, an empty list is
not an error — the handler decides whether that is good enough with a `require`.

## 19. `group`

```
group_decl   ::= "group" "(" STRING ")" ":" INDENT { group_member } DEDENT
group_member ::= require_stmt | route_decl | group_decl
```

```lum
group("/api/v1"):
    require jwt.valid else status(401)

    get endpoint("/me"):
        return { "sub": jwt.claims["sub"] }

    group("/admin"):
        require jwt.claims["role"] == "admin" else status(403)

        get endpoint("/stats"):
            return { "users": state.get("users", 0) }
```

The prefixes are concatenated (`/api/v1` + `/admin` + `/stats` = `/api/v1/admin/stats`) and
**the guards accumulate**: to reach the most nested route you have to pass the parent group's
first and then its own, in that order — guards always go **before** the routes inside a
`group` block, even if they appear interleaved with them in the source.

There is no *middleware* concept in Lumen Script: `require expr else response` (§27) is the
only way to intercept a request before it reaches the handler, and it is a conditional
expression with one exit, not a chain of functions that can be composed arbitrarily. The
reason is in [README.md](README.md#runs-behind-a-reverse-proxy): CORS, compression, *rate
limiting* and security headers are delegated to the reverse proxy on purpose, and the only
thing left for the language to solve was "can this request go on, or not?" — which is exactly
what `require` expresses.

## 20. `on error`

```
error_decl ::= "on" "error" [ INT ] ":" block
```

```lum
on error 404:
    return render("404.html", path=request.path, method=request.method)

on error 403:
    return { "error": "you cannot come in here", "code": error.code }

on error:
    log.error(error.message)
    return render("500.html")
```

Without a code, it is the global handler — the one that catches any error status with no more
specific one declared. The code, if given, has to be between **400 and 599**: with a `2xx` the
route's handler has already written the response, and replacing it from outside would be a
response filter — that is, half of what a *middleware* does, which Lumen Script avoids on
purpose (§19).

Inside the block the reserved object `error` exists (§45). In an `on error 422`,
`error.messages` carries the complete list of messages from a failed `validate:` —empty if the
422 did not come from there:

```lum
on error 422:
    return { "details": error.messages }
```

The response status code is preserved as is, even if the handler changes the body. If the
handler writes nothing, the default error body is kept.

---

# Part IV — Statements

```
block      ::= INDENT { statement } DEDENT
statement  ::= var_decl | assign_stmt | if_stmt | while_stmt | for_stmt
             | return_stmt | require_stmt | try_stmt
             | "break" NEWLINE | "continue" NEWLINE | expr NEWLINE
```

## 21. Variable declaration

```
var_decl ::= type IDENT [ "=" expr ] NEWLINE
```

```lum
int    n = 5
string name
List<int> xs = [1, 2, 3]
```

A variable declared without an initializer holds its type's default value (`0` for the
numeric ones, `""` for `string`, `false` for `bool`, `null` for an optional or class type).

## 22. Assignment

```
assign_stmt ::= lvalue ( "=" | "+=" | "-=" | "*=" | "/=" | "%=" ) expr NEWLINE
lvalue      ::= IDENT { "." IDENT | "[" expr "]" }
```

```lum
n = n + 1
n += 1              # sugar for the above
obj.field = 5
list[0] = 99
map["key"] += 1
list[0] += 10       # the compound form works on an index too
```

`x += e` is exactly sugar for `x = x + e` — there is no different semantics for types that
overload `+` (there is no operator overloading in Lumen Script), so `+=` on a `string`
concatenates and on a `List` it appends the elements of another list:

```lum
string s = "hel"
s += "lo"                 # "hello"

List<int> l = [1]
l += [2, 3]                # [1, 2, 3]
```

## 23. `if` / `elif` / `else if` / `else`

```
if_stmt ::= "if" expr ":" block
            { ( "elif" | "else" "if" ) expr ":" block }
            [ "else" ":" block ]
```

`elif` and `else if` are interchangeable — the grammar accepts both forms on purpose, so as
not to break code written by someone coming from a language with one convention or the other:

```lum
get endpoint("/elif/:n", int n):
    if n < 0:
        return { "r": "negative" }
    elif n == 0:
        return { "r": "zero" }
    else:
        return { "r": "large" }

get endpoint("/elseif/:n", int n):
    if n == 1:
        return { "r": "one" }
    else if n == 2:
        return { "r": "two" }
    else:
        return { "r": "another" }
```

There is no dangling `else` ambiguity: since blocks are made by indentation and not by `{ }`,
an `else` **always** closes the `if` at its own column. C's classic problem
(`if (a) if (b) ...; else ...;` — which `if` does the `else` belong to?) does not exist
because there is no way to write it ambiguously.

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

It walks a `List<T>` by value, or the **keys** of a `Dict<K,V>` (the type declared in the
`for` must match `K`, almost always `string`):

```lum
List<int> xs = [1, 2, 3, 4, 5]
int total = 0
for int x in xs:
    total = total + x

Dict<string,int> d = { "one": 1, "two": 2 }
List<string> keys = []
for string k in d:
    keys = keys + [k]
```

There is only one form of `for` — no C-style `for (init; cond; incr)` and no separate
`range()`. A counted loop is written with `while` (§24) or by walking a range built by hand;
iterating over something that is neither a `List` nor a `Dict` is a compile error:

```
./app.lum:3:14: error: cannot iterate over int
```

## 26. `return`

```
return_stmt ::= "return" [ expr ] NEWLINE
```

Covered in detail in Part VII. `return` with no expression exits with `204` when a `fn`'s
return type is `void`; inside a handler, a `return` with no value is equivalent to
`return status(204)`.

## 27. `require ... else`

```
require_stmt ::= "require" expr "else" expr NEWLINE
```

```lum
require image.content_type.starts_with("image/") else status(415)
require image.size <= 5 * 1024 * 1024             else status(413)
```

Exact sugar for:

```lum
if not (image.content_type.starts_with("image/")):
    return status(415)
```

It is not a *middleware* construct: it is an early return like any other, and that is why it
works in **any position** a statement works — inside a loop, inside an `if`, in the middle of
a function, not only at the start of a route or a group (§19).

## 28. `try`/`catch`

```
try_stmt ::= "try" ":" block "catch" [ IDENT ] ":" block
```

```lum
get endpoint("/basic/:n", int n):
    try:
        int x = n / 0
        return { "no": x }
    catch e:
        return { "caught": e.message }

get endpoint("/no_variable/:n", int n):
    try:
        int x = 1 % n
        return { "ok": x }
    catch:
        return { "failure": true }
```

The name after `catch` is optional; if given, inside the `catch` block it is a value with a
single useful field, `.message`. It is resolved with a table of bytecode ranges computed at
compile time, not with a runtime handler stack, so:

- A `return` **inside** the `try` leaves the function normally and does not trigger the
  `catch`.
- A `return` cannot leave a handler "dangling" for later: as soon as execution leaves the
  `try`'s range —by whatever path— that `catch` stops being active.

  ```lum
  get endpoint("/no_leak/:n", int n):
      int acc = 0
      for int i in [1, 2]:
          try:
              acc = acc + i
          catch e:
              acc = -1
      # This failure is OUTSIDE the try above: it has to blow up, not be caught.
      int bad = acc / 0
      return { "no": bad }
  ```

- Nested, the innermost one wins:

  ```lum
  get endpoint("/nested"):
      try:
          try:
              int x = 1 / 0
          catch e:
              return { "inner": e.message }   # this one catches
      catch e:
          return { "outer": e.message }       # this one never sees it
  ```

- Execution can carry on after catching, without leaving the handler:

  ```lum
  get endpoint("/carries_on"):
      int total = 0
      for int d in [2, 0, 5]:
          try:
              total = total + 100 / d
          catch e:
              log.warn("skipped: " + e.message)
      return { "total": total }
  ```

An error caught by no `try` climbs until it becomes the default error response —or the `on
error` that applies (§20)— shaped as `{"error": message, "en": file:line:column}` with status
`500`.

## 29. `break` and `continue`

```lum
for int x in [1, 2, 3, 4, 5]:
    if x == 3:
        break
    if x % 2 != 0:
        continue
    # ...
```

Only valid inside a `while` or a `for`. Using them outside is a compile error, not a
meaningless effect at run time.

## 30. `++` and `--` as expressions

`++` and `--` are **not only statements**: they are expression operators, with the two usual
forms, and they work on any postfix position — a variable, a field, or an indexed element.

```
unary   ::= [ "-" | "++" | "--" ] postfix
postfix ::= primary { "." IDENT | "(" [ args ] ")" | "[" expr "]" | "++" | "--" }
```

```lum
int i = 5
int a = i++          # a = 5, i = 6   -- postfix: gives the value BEFORE incrementing
int b = ++i          # b = 7, i = 7   -- prefix:  gives the value AFTER
int c = i--          # c = 7, i = 6
int d = --i          # d = 5, i = 5
```

On a field:

```lum
class Box:
    int n

get endpoint("/field"):
    Box c = Box(10)
    int old = c.n++              # old = 10, c.n = 11
    int fresh = ++c.n            # fresh = 12, c.n = 12
    return { "old": old, "fresh": fresh, "n": c.n }
```

On a list index, chained with the `[ ]` itself:

```lum
List<int> l = [10, 20, 30]
int i = 0
int first   = l[i++]        # first = 10, i becomes 1
int second  = l[i++]        # second = 20, i becomes 2
```

As a bare statement, the value it produces is simply discarded:

```lum
i++
++i
i--
```

And inside a `while` condition, exactly as in C:

```lum
int total = 0
int k = 0
while k < 5:
    total += k++
```

---

# Part V — Expressions

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

## 31. Full precedence

Lowest to highest — one real example per level:

| # | Operator | Associativity | Example |
|---|---|---|---|
| 1 | `?:` | right | `age >= 18 ? "adult" : "minor"` |
| 2 | `or` | left | `a == 1 or b == 2` |
| 3 | `and` | left | `a == 1 and b == 2` |
| 4 | `not` | (unary) | `not found` |
| 5 | `== !=` | left | `status != "closed"` |
| 6 | `< <= > >=` | left | `age >= 18` |
| 7 | `+ -` | left | `price - discount` |
| 8 | `* / %` | left | `total * 1.21` |
| 9 | `-` (unary) | — | `-balance` |
| 10 | `. ( ) [ ]` `++` `--` | left | `orders[0].total()` |

`1 + 2 * 3` is `7`, not `9` — arithmetic respects the usual mathematical precedence, and
`*`/`/`/`%` bind tighter than `+`/`-` exactly as in any other language with this family of
operators.

## 32. Ternary

```lum
string role = is_admin ? "admin" : "user"
```

Right-associative and the lowest precedence of all — it wraps practically any expression
without needing parentheses around either branch:

```lum
return rows == 0 ? status(404) : status(204)
```

## 33. Booleans and truthiness

`and`, `or` and `not` compile to bytecode jumps with short-circuiting, as in Python or C: in
`a() and b()`, if `a()` is false, `b()` **is never evaluated**. This matters when one of the
two sides has side effects —a database call, an increment— and not only when it is a pure
check.

In boolean context (`if`, `while`, `require`, `and`, `or`, `not`, a ternary's condition) the
**false** values are: `null`, `false`, `0`, `0.0`, the empty string `""`, and the empty list
and dictionary. Everything else is true. It is Python's rule, which is where most people
writing Lumen Script for the first time are coming from.

See also §9 for the `if x:` trap with an optional value.

## 34. Equality and comparison

**There is no type coercion inside the operators.** `0 == "0"` is `false` in Lumen Script, not
`true` as in JavaScript, and not a compile error either — it simply compares an integer
against a string and the result is that they are not equal.

```lum
0 == "0"        # false, no error raised
1 + "1"         # COMPILE ERROR: cannot add int and string
```

To concatenate a number with text you have to convert it explicitly:

```lum
"n = " + str(n)
```

`<`, `<=`, `>`, `>=` are only defined between numeric types (`int`/`long`/`float`/`double`,
interchangeable with each other, §7) and not between `string`s — there is no built-in
lexicographic ordering of strings in the comparison operators.

## 35. Arithmetic, without coercion

`+`, `-`, `*`, `/`, `%` work between the numeric types, and `+` also concatenates two
`string`s or two `List<T>` with the same element type. Any other combination is an error **at
compile time**, not a silent implicit conversion and not a run-time `NaN`:

```lum
1 + "1"             # compile error
"a" - "b"           # compile error: '-' does not apply to string
[1, 2] + [3]        # [1, 2, 3] — concatenation, valid
```

## 36. List and dictionary literals

```lum
List<int> xs = [1, 2, 3]
List<int> empty = []

Dict<string,int> d = { "one": 1, "two": 2 }
Dict<string,int> empty_map = {}
```

A dictionary literal is never confused with a statement block: blocks are marked by
indentation (§1), so `{` **always** opens a dictionary, in any position where an expression
is valid. There is no need to disambiguate by context as in JavaScript, where `{}` at the
start of a statement is an empty block and in any other position an empty object.

Every key of a `dict_literal` is evaluated at run time and must yield a `string`; see §8 for
the exact error the opposite produces.

## 37. Indexing

```lum
List<int> l = [10, 20, 30]
l[0] = 99                    # indexed assignment
l[2] = l[1] + 5              # the right-hand side can read from the same container
l[0] += 10                   # compound, on an index

Dict<string,int> d = { "a": 1 }
d["b"] = 2
d["a"] = d["a"] + 10
```

**A `List` index has to be an `int`**, and within range — out of range is a run-time error,
not a silent `null`:

```
{"error":"index out of range: 5 (size 1)"}
```

**A `Dict` key that does not exist, on the other hand, gives `null`** when read with `[ ]` —
it raises no error. It is a deliberate asymmetry between the two containers: a list has a
known size and running off it is almost always a handler bug that is better off showing up
loudly; a dictionary is often used as a sparse map where "the key is not there" is a normal
business case, not a programming error.

```lum
Dict<string,int> d = { "a": 1 }
d["z"]          # null, not an error
List<int> l = [1]
l[5]            # {"error": "index out of range: 5 (size 1)"}
```

## 38. Calls, positional and named arguments

```
args ::= arg { "," arg }
arg  ::= expr | IDENT "=" expr
```

```lum
render("page.html", title="T", note="N")
```

Named arguments go **after** the positional ones, never before or interleaved — `f(a=1, b)`
is not valid, `f(b, a=1)` is. A parameter with a default value that is not named explicitly
is filled in positionally like any other.

## 39. Postfix chaining

```
postfix ::= primary { "." IDENT | "(" [ args ] ")" | "[" expr "]" | "++" | "--" }
```

Everything that follows a value —field access, call, index, increment— chains in the same
syntactic position, left to right, with no depth limit:

```lum
orders[0].customer.address.city.upper()
render("x.html").status(203).header("X-Cache", "miss")
```

And **every step of the chain is checked at compile time** if the receiver's type is known at
that point — see Part VI.

## 40. `await` and async

```lum
get endpoint("/slow/:ms", int ms):
    await sleep(ms)
    return { "waited": ms }
```

`await` is only valid inside a route handler, and only on an expression the checker knows is
*suspendable*. The suspendable ones are: `sleep(ms)`, `ws.recv()`, and every method of the
database modules (`query`, `exec`, `begin`, `commit`, `rollback`, `last_id` — §46).

Three rules checked **at compile time**, not in production:

```lum
sleep(100)             # ERROR: 'sleep()' is asynchronous: you must write 'await sleep(...)'
await text("x")        # ERROR: 'text()' is not asynchronous: the 'await' is unnecessary
await 5                # ERROR: 'await' only applies to an asynchronous call
```

`await` suspends the whole handler and hands control back to that thread's *event loop*; the
rest of the connections served by the same thread keep advancing meanwhile. Eight concurrent
requests doing `await sleep(500)` take 500 ms in total, not four seconds — that is the entire
difference between an asynchronous server and one that blocks a thread per request.

`sleep()` wakes early if the client disconnects; in that case the handler does not keep
running after the `await`.

Inside, this is what forces the VM to have its own stack and locals instead of using C++'s
call stack: an interpreter that recurses on the native stack cannot stop halfway and hand
control somewhere else. Every in-flight request lives in its own VM frame, held inside the
handler's coroutine.

---

# Part VI — Checked at compile time, not at run time

## 41. Methods, fields and types are resolved at compile time

When the receiver's type is known at the call site —a parameter with a declared type, a
variable declared with a type, a literal— the method name and the argument count are checked
**there**, not the first time that code runs:

```lum
get endpoint("/a", string who):
    return { "r": who.mayusculas() }
```

```
./app.lum:2:19: error: values of type string have no method 'mayusculas';
                  it has status, header, cookie, starts_with, ends_with, contains,
                  upper, lower, trim
```

The check continues **down the whole chain**, because every method knows what type it
returns: `s.upper().recortar()` also fails at compile time, at the exact position of
`.recortar()`. The same goes for a class's fields:

```lum
class Point:
    int x
    int y

get endpoint("/z"):
    Point p = Point(1, 2)
    return { "z": p.z }
```

```
./app.lum:7:16: error: 'Point' has no field 'z'; it has x, y
```

**This reaches inside the templates too**, because `render()` hands the template engine the
types of the arguments it was called with, and the template engine checks them with the same
checker as the rest of the language: `{{ who.mayusculas() }}` is a `lumen --check` error with
the file and the line **of the template**, not an exception halfway through rendering with
half the page already sent to the client.

Where the receiver's type is **not** known statically —the variable of a `for` over a `Json`,
a field read from a `Dict<string,Json>`— there is nothing to check and dispatch stays at run
time, as in any dynamic language.

The complete table of available methods, by receiver type:

| Receiver | Methods |
|---|---|
| Any returned value | `status(code)` `header(k, v)` `cookie(k, v, ...)` |
| `string` | `starts_with(s)` `ends_with(s)` `contains(s)` `upper()` `lower()` `trim()` |
| `List<T>` | `add(v)` |
| `Dict<K,V>` | `has(key)` `keys()` |
| `File` | `save(directory)` |

## 42. Bidirectional inference

The expected type on the left-hand side of an assignment **flows down** into the literal on
the right:

```lum
List<string> xs = []
```

The `[]` on the right takes its element type (`string`) from the `List<string>` on the left.
Without this you would have to write something like `List<string> xs = List<string>()`, which
is exactly the ceremony Lumen Script tries to avoid in every language design decision — see
[README.md](README.md#design-decisions) for the rest of the decisions and their reasons.

---

# Part VII — Returning from a handler

## 43. Forms of `return`

Everything that leaves a handler leaves through `return`. There is no mutable `response`
object to fill in and carry around the function body.

| Form | Result |
|---|---|
| `return <class-instance>` | `200`, body serialized to JSON |
| `return { ... }` / `return [ ... ]` | `200`, JSON body |
| `return render("x.html", k=v, ...)` | `200`, HTML rendered with the Lumen Script template engine |
| `return text("...")` | `200`, `text/plain` |
| `return html("...")` | `200`, `text/html` |
| `return send_file(path)` | The file, served with `sendfile(2)` — without copying through user space |
| `return send_file(path, root)` | Same, but `path` is confined inside `root` (symlinks resolved, `..` rejected) |
| `return redirect(path)` | `302` |
| `return redirect(path, code)` | The given code, typically `301` |
| `return status(code)` | The status code, no body |
| `return` (no expression) | `204` |

```lum
get endpoint("/download"):
    return send_file("/var/files/report.pdf")

get endpoint("/files/:name"):
    # :name comes from the client — send_file(path) alone would let it read
    # anything the process can, e.g. name=/etc/passwd or name=../app.lum.
    # The two-argument form confines it inside "./uploads" instead.
    return send_file(name, "./uploads")

get endpoint("/legacy"):
    return redirect("/new", 301)
```

`send_file(path)` (one argument) trusts `path` completely, the same way a hardcoded string
literal is trusted — never call it with `query(...)`, a path param, or any other value that
came from the request. Use `send_file(path, root)` for that; `redirect(path)` has the same
rule (a redirect target taken straight from request input, with nothing validated, is an open
redirect).

## 44. Chaining: `status`, `header`, `cookie`

Every form of `return` accepts chaining these three calls, in any order and any combination:

```lum
return { "id": 1 }.status(201)
return { "a": 1 }.header("X-Thing", "value")
return { "n": 1 }.status(202).header("X-One", "1").header("X-Two", "2")
return render("x.html").status(203)

return { "ok": true }.cookie("theme", "dark",
                             max_age=3600, http_only=false, same_site="strict")
```

`cookie` options: `max_age`, `path`, `domain`, `secure`, `http_only`, `same_site`
(`"lax"` | `"strict"` | `"none"`). Defaults: `path=/`, `HttpOnly` on, `SameSite=Lax`.

---

# Part VIII — Reserved objects

## 45. The complete surface

| Object | Members | Where |
|---|---|---|
| `request` | `path` `method` `ip` | Any handler |
| `session` | any field (free store), `clear()` | Any handler, with `session:` configured in `app:` |
| `jwt` | `valid` `claims` | Any handler, under a `jwt` guard |
| `state` | `incr(k[,n])` `decr(k[,n])` `get(k[,def])` `set(k,v)` `remove(k)` | Any handler and function |
| `log` | `info(msg)` `warn(msg)` `error(msg)` | Everywhere |
| `sse` | `send(data)` `send(event,data)` `send(event,data,id)` `ping([text])` `open` | `sse` routes |
| `ws` | `send(msg)` `await recv()` `open` `close()` | `ws` routes |
| `error` | `code` `message` `messages` | `on error` blocks |
| `this` | the class's fields and methods | Methods and constructors |

`session` is a store of free fields, not a class with a fixed shape — `session.<whatever>`
accepts any name, and reading one that was never written gives `null`:

```lum
post endpoint("/login", Login data):
    session.user = data.name
    session.role = data.name == "alice" ? "admin" : "user"
    return { "ok": true }

get endpoint("/who"):
    return { "user": session.user, "role": session.role }
```

It travels in a cookie signed with HMAC-SHA256 —signed, **not encrypted**: the client can
read the content, they just cannot forge it— so nothing the user should not see goes there.
An invalid signature leaves the whole session empty, never half-filled.

`state` is the only shared-state path between the different *event loop* threads: each one
has its own VM with its own stack and its own heap, and they share no memory. That is why
`state` exposes **operations** (`incr`, `decr`) instead of a property you could read and
rewrite by hand — writing `state.x = state.x + 1` would be a read followed by a write racing
between two threads, while `state.incr("x")` is a single atomic operation that also returns
the already updated value. It lives in process memory: it is lost on restart and is not
shared between different machines.

`jwt.claims` and the result of a database query are `Json` values: they are read with `[ ]`,
never with `.`, because a `Json` has no fixed fields the checker could verify:

```lum
get endpoint("/me"):
    return { "sub": jwt.claims["sub"], "role": jwt.claims["role"] }
```

## 46. The database modules

`sqlite`, `postgres` and `mysql` behave as reserved objects once imported and configured
(§13, §14): `query`, `exec`, `begin`, `commit`, `rollback`, and `last_id` —that last one only
in `sqlite` and `mysql`, because postgres has no reliable equivalent and the module says so
instead of making one up. Every one of their methods is asynchronous: they are always called
with `await` (§40).

```lum
get endpoint("/articles/:id", int id):
    List<Json> rows = await sqlite.query(
        "select title from articles where id = ?", id)
    if len(rows) == 0:
        return status(404)
    return rows[0]
```

`query()` returns `List<Json>`. `exec()` returns the number of affected rows, as an `int`. An
engine error —a table that does not exist, a constraint violation— **does not blow up the
handler**: it arrives as a `Json` value with the `error` key, so the handler decides what to
do with it instead of the failure jumping straight to a generic `500`:

```lum
get endpoint("/bad"):
    Json r = await sqlite.query("select * from table_that_does_not_exist")
    return r          # { "error": "no such table: table_that_does_not_exist" }
```

**Parameters always travel separately from the query text, never concatenated** — that is the
only way to open yourself to a SQL injection, and the language gives no convenient way to get
it wrong. The placeholder is `?` in all three engines: although postgres numbers its own
internally (`$1`, `$2`...), the driver itself translates the query before sending it, so the
same string works without changing a letter in `sqlite`, `mysql` and `postgres`:

```lum
await sqlite.query(  "select title from articles where id = ?", id)
await mysql.query(   "select title from articles where id = ?", id)
await postgres.query("select title from articles where id = ?", id)
```

A query already written with `$1` reaches postgres untouched, so code written before this
translation keeps working unchanged. A `?` inside a string, a quoted identifier, a comment, or
a `$$...$$` block is left alone. Mixing `?` and `$1` in the same query is an error, because
the numbering would clash.

Transactions: `begin()` pins the pool connection for the rest of the request, and
`commit()`/`rollback()` release it. If the handler ends —or blows up— with a transaction open,
Lumen issues a `ROLLBACK` on its own and warns on the console; without that, the next request
reusing that pool connection would inherit a half-finished transaction that is not its own.

```lum
post endpoint("/transfer"):
    await sqlite.begin()
    await sqlite.exec("update accounts set balance = balance - 30 where name = ?", "ana")
    await sqlite.exec("update accounts set balance = balance + 30 where name = ?", "bob")
    await sqlite.commit()
    return { "ok": true }
```

---

# Part IX — Ambiguities and parser decisions

## 47. Table of ambiguities

| Case | Resolution |
|---|---|
| `>>` when closing nested generics | The lexer never merges `>` `>` into a single token (§6) |
| Optional-type `?` versus the ternary | One token of *lookahead*: after `?`, an `IDENT` means a declaration (§11) |
| `{` of a statement block versus a dictionary literal | Blocks are always by indentation; `{` in expression position always opens a dictionary (§36) |
| Dangling `else` | Impossible: blocks are by indentation, there is no `{ }` to disambiguate (§23) |
| `elif` versus `else if` | Both forms are valid and equivalent (§23) |

---

# 48. Three complete programs

## Login with a session and a role-protected area

```lum
app:
    port      8090
    templates "./templates"

    session:
        secret  env("SESSION_SECRET")
        max_age 3600
        secure  false          # locally, with no TLS in front

    jwt:
        secret env("JWT_SECRET")
        issuer "my-app"


class Login:
    string name
    string password

    validate:
        name != ""      "name: required"
        password != ""  "password: required"


post endpoint("/login", Login data):
    if data.password != "hunter2":
        return status(401)

    session.user = data.name
    session.role = data.name == "alice" ? "admin" : "user"
    return { "ok": true, "role": session.role }

get endpoint("/who"):
    return { "user": session.user, "role": session.role }

post endpoint("/logout"):
    session.clear()
    return { "ok": true }


group("/admin"):
    require session.role == "admin" else status(403)

    get endpoint("/panel"):
        return { "panel": true, "of": session.user }

    # Groups nest and guards accumulate: to reach here you have to pass the
    # parent's first.
    group("/danger"):
        require session.user == "alice" else status(403)

        get endpoint("/button"):
            return { "pressed": true }


group("/api"):
    require jwt.valid else status(401)

    get endpoint("/me"):
        return { "sub": jwt.claims["sub"], "role": jwt.claims["role"] }
```

## CRUD with transactions on sqlite

```lum
import sqlite

app:
    port 8070
    sqlite:
        file "./accounts.db"
        pool 4

class Account:
    string name
    int    balance

    validate:
        name != ""      "name: required"
        balance >= 0    "balance: cannot be negative"

get endpoint("/balances"):
    return await sqlite.query("select name, balance from accounts order by name")

post endpoint("/accounts", Account c):
    int rows = await sqlite.exec(
        "insert into accounts (name, balance) values (?, ?)", c.name, c.balance)
    int id = await sqlite.last_id()
    return { "id": id, "rows": rows }.status(201)

# Atomic transfer: both updates or neither.
post endpoint("/transfer/:amount", int amount):
    await sqlite.begin()
    await sqlite.exec("update accounts set balance = balance - ? where name = 'ana'", amount)
    await sqlite.exec("update accounts set balance = balance + ? where name = 'bob'", amount)
    await sqlite.commit()
    return { "moved": amount }

# Undone by hand if something does not add up.
post endpoint("/undone/:amount", int amount):
    await sqlite.begin()
    await sqlite.exec("update accounts set balance = balance - ? where name = 'ana'", amount)
    await sqlite.rollback()
    return { "undone": true }

on error 422:
    return { "details": error.messages }

on error:
    log.error(error.message)
    return { "error": "something broke", "en": request.path }
```

## Real time: SSE and WebSocket over shared state

```lum
app:
    port 8087

get endpoint("/counter"):
    return { "count": state.get("counter", 0) }

post endpoint("/counter/incr"):
    return { "count": state.incr("counter") }


sse endpoint("/counter/live"):
    sse.send("snapshot", { "count": state.get("counter", 0) })

    int tick = 0
    while sse.open:
        await sleep(2000)
        tick = tick + 1
        sse.send("delta", { "count": state.get("counter", 0), "tick": tick })
        if tick % 10 == 0:
            sse.ping("keepalive")


ws endpoint("/counter/ws") origins("https://myapp.com", "http://localhost:5173"):
    ws.send({ "count": state.get("counter", 0) })

    while ws.open:
        Json msg = await ws.recv()
        if msg == null:                 # the client closed the connection
            break

        string action = msg["action"]
        if action == "increment":
            ws.send({ "count": state.incr("counter") })
        elif action == "decrement":
            ws.send({ "count": state.decr("counter") })
        elif action == "reset":
            state.set("counter", 0)
            ws.send({ "count": 0 })
```

---
# Appendix A — The complete formal grammar

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

INT            ::= digit { digit }
FLOAT          ::= digit { digit } "." digit { digit }
STRING         ::= '"' { character } '"' | '"""' { character | break } '"""'
BOOL           ::= "true" | "false"
NULL           ::= "null"
IDENT          ::= ( letter | "_" ) { letter | digit | "_" }
```

---

# Appendix B — Reserved words

```
import  class  fn  app  group  endpoint  on  error  origins
get  post  put  patch  delete  any  sse  ws
if  else  elif  while  for  in  return  require  try  catch  break  continue
validate  and  or  not  true  false  null  this  void  spa
```

Reserved objects (not keywords; they exist only inside their context — see §2, §45):
`request` `session` `state` `jwt` `sse` `ws` `error` `log` `this`.

---

# Appendix C — Error messages quoted in this book

| Message | Section | What is missing |
|---|---|---|
| `the pattern declares ':id' but no parameter binds it` | §17 | The parameter is missing from the signature |
| `'sleep()' is asynchronous: you must write 'await sleep(...)'` | §40 | The `await` is missing |
| `'text()' is not asynchronous: the 'await' is unnecessary` | §40 | The `await` is redundant |
| `'await' only applies to an asynchronous call` | §40 | `await` on something that does not suspend |
| `'sse' only exists inside an sse route` | §2 | A reserved object out of context |
| `a ws route needs origins(...)` | §17 | The origin allowlist is missing |
| `cannot add int and string` | §35 | An arithmetic operation between incompatible types |
| `the session is not configured` | §45 | `session: secret ...` is missing from `app:` |
| `'X' is not declared` | §15 | An unknown name, inside `validate` too |
| `values of type string have no method 'M'; it has ...` | §41 | A non-existent method on a known type |
| `'Class' has no field 'X'; it has ...` | §41 | A non-existent field on a known class |
| `a Dict key must be a string` | §8, §36 | A `Dict` literal, read or write with a non-string key |
| `index out of range: N (size M)` | §37 | A `List` access outside its bounds |
| `module 'X' is not compiled into this binary` | §13 | An `import` of a database module that is not linked |

Every compile error comes out with file, line, column, and a cursor under the exact position
— there is no generic "there is an error somewhere in your program" message.
