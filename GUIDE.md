# Lumen — Developer guide

> A practical reference for Lumen Script, the language Lumen runs. The formal grammar is in
> [LUMEN_SCRIPT-GRAMMAR.md](LUMEN_SCRIPT-GRAMMAR.md); the design decisions and their reasons,
> in [README.md](README.md#design-decisions).

## Contents

1. [Getting started](#1-getting-started)
2. [Project layout](#2-project-layout)
3. [The `app:` block](#3-the-app-block)
4. [Routes](#4-routes)
5. [Parameters](#5-parameters)
6. [Classes and validation](#6-classes-and-validation)
7. [Responses](#7-responses)
8. [Groups and guards](#8-groups-and-guards)
9. [Session](#9-session)
10. [JWT](#10-jwt)
11. [Async](#11-async)
12. [Databases](#12-databases)
13. [Server-Sent Events](#13-server-sent-events)
14. [WebSockets](#14-websockets)
15. [Shared state](#15-shared-state)
16. [File uploads](#16-file-uploads)
17. [Error handlers](#17-error-handlers)
18. [Functions](#18-functions)
19. [The language](#19-the-language)
20. [Builtin reference](#20-builtin-reference)
21. [Common errors](#21-common-errors)
22. [How it works inside](#22-how-it-works-inside)

---

## 1. Getting started

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Requires Linux (epoll, `sendfile(2)`, `SO_REUSEPORT`), CMake 3.20+ and C++20. The first
`configure` needs no network: nothing is downloaded.

Optional but recommended: `sudo apt install libjemalloc-dev`. With several event loops and a
database pool, glibc's `malloc` serializes on its arenas; swapping it is worth more than any
code optimization on large JSON responses. If it is there, `cmake` links it automatically; if
not, it builds anyway and says so.

The argument decides what gets compiled, with no surprises:

| Invocation | What it compiles |
|---|---|
| `lumen app.lum` | Just that file |
| `lumen a.lum b.lum` | Just those two |
| `lumen ./my-app` | Every `.lum` in the directory, recursively |

| Option | |
|---|---|
| `--check` | Compile and exit, without starting |
| `--port N` | Override the port from the `app:` block |
| `--no-watch` | Do not watch for changes |
| `--verbose` | Log every incoming request to the console. It costs ~25% of the throughput, so it is off by default |
| `--autotest` | Walk the endpoints on startup and on every reload |
| `--autotest=all` | Also include POST/PUT/PATCH/DELETE |

### Self-test

With `--autotest`, after startup and after **every successful reload**, Lumen talks to itself
over HTTP and walks the module's routes:

```
changes detected: recompiling
reloaded: 11 route(s) — 4 declarative, 7 with logic
autotest: probing 11 route(s)
  ok        GET    /users/1                          200  0ms
  ERROR     GET    /broken                           500  0ms
  rejected  GET    /admin/panel                      403  0ms
  stream    GET    /events                           200  0ms
  skipped   WS     /chat   (needs a WebSocket handshake)
autotest: 8 ok, 2 rejected, 1 with an error, 1 skipped
```

It does not check business logic: it looks for **no handler breaking** after a change. That
is why a `5xx` is the only thing that counts as an error — a `4xx` can be the correct
behaviour of a guard or a validation.

Route parameters are filled in by type (an `:id` of type `int` → `1`), and with
`--autotest=all` the body is synthesized from the class the route expects.

**On side effects:** probing an endpoint *runs its handler*. A `DELETE` would really do its
work on every reload, so by default only `GET`, `HEAD` and `SSE` are walked. Including the
rest is a decision of whoever launches the binary, not of the binary.

The watcher watches exactly the set that was compiled. On save, it recompiles and replaces
the module. **If the new file does not compile, the previous one keeps serving** and the
error is printed with file, line and column.

---

### Lumen's own tests

```bash
cd build && ctest --output-on-failure
# or directly:
tests/run_tests.sh ~/lumen-build/lumen
```

246 tests in six suites:

| suite | what it covers | |
|---|---|---|
| `regression` | the binary over the socket, with real `.lum` files | 79 |
| `templates` | compiling and rendering, in process | 48 |
| `sqlite` | types, limits and the statement cache | 37 |
| `postgres` | types, placeholders, transactions and concurrency with checked content | 34 |
| `mysql` | the same, plus the null byte inside a text | 29 |
| `placeholders` | the translation of `?` to `$1` | 19 |

The `mysql` and `postgres` ones need a server and **skip themselves** if there is none; the
instructions for setting one up are in each script's header. The `sqlite` one creates its own
schema and always runs. The suite links nothing from the project: it tests what gets
deployed, not an instrumented version of it.

---

## 2. Project layout

```
my-app/
  app.lum           configuration
  routes/
    public.lum
    admin.lum
  templates/         Lumen Script templates
  public/            static files
```

The order between files does not matter: compilation runs in two passes, first the
declarations are collected and then the names are resolved. A class can be used before it is
declared, and can live in another file.

---

## 3. The `app:` block

It can be in any file, but **only once**.

```lum
app:
    name      "My application"
    version   "1.0.0"
    port      8080
    templates "./templates"

    static "/static" -> "./public"
    static "/"       -> "./dist" spa

    docs                      # /openapi.json and /docs
    health                    # /health
    metrics                   # /metrics, Prometheus format

    session:
        secret  env("SESSION_SECRET")
        max_age 86400
        secure  true

    jwt:
        secret env("JWT_SECRET")
        issuer "my-app"
```

`env("VAR")` is resolved **at compile time**. It is how a secret avoids ending up written in
the `.lum`.

`spa` on a static mount makes routes that are not found fall back to `index.html`.

---

## 4. Routes

```lum
get    endpoint("/path"):
post   endpoint("/path"):
put    endpoint("/path"):
patch  endpoint("/path"):
delete endpoint("/path"):
any    endpoint("/path"):
sse    endpoint("/path"):                       # event stream
ws     endpoint("/path") origins("https://x"):  # WebSocket
```

Patterns: `/users/:id`, `/users/{id}`, `/files/*`.

### The two route levels

A route whose body is resolved entirely at compile time —a single `return` of a constant
value or of a native call with literal arguments— becomes a **native action** and does not
run a single bytecode step:

```lum
get endpoint("/"):
    return render("index.html")        # declarative: zero bytecode
```

The rest run bytecode. On startup, the binary says how many take each path:

```
lumen: 3 file(s), 12 route(s) — 5 declarative, 7 with logic
```

A route with group guards is **never** declarative: the native action would not run them.

---

## 5. Parameters

Everything the handler needs is declared in the signature.

```lum
get endpoint("/users/:id", int id, int page = 1, string q):
    return { "id": id, "page": page, "q": q }
```

| Form | Where it comes from |
|---|---|
| A name that appears in the pattern | Route segment |
| A name that does not appear | Query string |
| `= value` | Default value if missing from the query |
| A type that is a `class` | JSON body, with validation |
| `File` / `List<File>` | Multipart parts |

Scalar types: `int`, `long`, `float`, `double`, `bool`, `string`.

**The compiler checks both directions**: that every `:name` in the pattern has a parameter
that binds it, and that no route parameter is left over.

A value that does not fit its type is a **400**, not an exception:

```json
{"error":"invalid parameter","expected":"int","param":"id","received":"abc"}
```

---

## 6. Classes and validation

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

Used as a parameter, it binds to the body:

```lum
post endpoint("/users", User u):
    # Here `u` is always valid.
    return { "created": u.name }
```

| Situation | Response |
|---|---|
| A body that is not JSON | `400 {"error":"invalid JSON"}` |
| A required field is missing | `422` with `"field: required"` |
| Wrong type | `422` with `"field: expected int"` |
| A `validate` rule fails | `422` with its message |

**Every message comes out at once**, not just the first. And the handler never runs.

There is no coercion: `"30"` in an `int` field is a 422, it is not parsed.

The `validate` rules **are compiled**, so a misspelled field in a rule is a compile error and
never reaches production:

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

    Point(int x, int y)        # without a body: each parameter goes to its field
```

They are told apart by the **number** of parameters. With none declared, one with every
field in declaration order is offered. Fields the constructor does not touch are `null`.

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

```lum
Point p = Point(3, 4)
p.squared()           # 25
p.label("Q")          # "Q(3,4)"
```

Methods and constructors compile as functions with `this` as the first parameter, so they use
the same frame stack and support recursion and default values just like `fn`.

The call **is resolved at compile time** from the receiver's declared type, so a misspelled
method never reaches production:

```
./app.lum:8:20: error: 'P' has no method 'triple'
```

An instance built by hand and one bound from the request body are the same thing: methods
work the same on both.

---

## 7. Responses

Everything goes out through `return`. There is no `response` object to carry around.

```lum
return { "key": "value" }                # 200, JSON
return [1, 2, 3]                         # 200, JSON
return render("page.html", k=v)          # HTML with Lumen Script templates
return text("hello")                     # text/plain
return html("<h1>hello</h1>")            # text/html
return send_file("/var/f.pdf")           # sendfile(2)
return redirect("/other")                # 302
return redirect("/other", 301)           # 301
return status(204)                       # status code, no body
```

A handler that returns nothing and writes no response produces **204**.

### Chaining

```lum
return { "id": 1 }.status(201)
return { "a": 1 }.header("X-Thing", "value")
return render("x.html").status(203)

return { "ok": true }.cookie("theme", "dark",
                             max_age=3600, http_only=false, same_site="strict")
```

`cookie` options: `max_age`, `path`, `domain`, `secure`, `http_only`, `same_site`
(`"lax"`, `"strict"`, `"none"`). Defaults: `path=/`, `HttpOnly`, `SameSite=Lax`.

---

## 8. Groups and guards

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

The prefixes are concatenated and **the guards accumulate**: to reach `/api/v1/admin/stats`
you have to pass the parent group's and then its own.

`require X else Y` is sugar for `if not X: return Y`. It works anywhere, not only inside a
group. There is no middleware concept.

Guards go **before** the routes inside the block.

---

## 9. Session

A cookie signed with HMAC-SHA256, Flask style. No server-side state.

```lum
post endpoint("/login", Login data):
    session.user = data.name
    session.role = "admin"
    return redirect("/")

get endpoint("/who"):
    return { "user": session.user, "role": session.role }

post endpoint("/logout"):
    session.clear()
    return redirect("/")
```

`session.<whatever>` accepts any name: it is a store, not an object with fixed fields. A
field that does not exist is `null`.

- It needs `session: secret ...` in the `app:` block. Without it, touching it is a runtime error.
- It is always **`HttpOnly`**; `Secure` depends on the configuration.
- The cookie is only rewritten if the handler modifies it.
- The content is **signed but not encrypted**: the user can read it, they just cannot forge
  it. Do not keep anything there they should not see.
- An invalid signature leaves the session empty, never half-filled.

---

## 10. JWT

HS256, verified against the `Authorization: Bearer ...` header.

```lum
group("/api"):
    require jwt.valid else status(401)

    get endpoint("/me"):
        return { "sub": jwt.claims["sub"], "role": jwt.claims["role"] }
```

Signature, `exp` and `iss` (if an `issuer` was configured) are checked. **Any `alg` other
than HS256 is rejected, `none` included**: accepting the algorithm the token itself declares
is the classic JWT library vulnerability.

RS256 is not there: it would require asymmetric cryptography, and Lumen does not link
OpenSSL.

---

## 11. Async

```lum
get endpoint("/slow/:ms", int ms):
    await sleep(ms)
    return { "waited": ms }
```

`await` suspends the handler and hands control back to the event loop. Eight concurrent
500 ms requests take 500 ms, not four seconds.

`sleep()` wakes early if the client disconnects, and in that case the handler does not carry
on.

Rules checked at compile time:

- An asynchronous builtin **requires** `await`: a bare `sleep(100)` is an error.
- A synchronous one **forbids** it: `await text("x")` is an error.
- `await` only applies to an asynchronous call: `await 5` is an error.

Available asynchronous builtins: `sleep(ms)` and `ws.recv()`.

---

## 12. Databases

Three modules: `sqlite`, `postgres` and `mysql`. You import and configure them; they manage
the connection.

```lum
import postgres

app:
    postgres:
        host     "127.0.0.1"
        port     5432
        database "my_app"
        user     "lumen_script"
        password env("PG_PASSWORD")
        pool     4
```

Each module is only compiled if its client was present when Lumen was built. If not,
`import postgres` gives an error when compiling the `.lum`, not a strange failure in
production.

### Configuration

| Module | Keys |
|---|---|
| `sqlite` | `file` (required), `pool`, `timeout_ms` |
| `postgres` | `url`, or else `host` / `port` / `database` (required) / `user` / `password`; `pool` |
| `mysql` | `host` / `port` / `database` / `user` / `password`; `pool` |

`pool` is the number of connections, between 1 and 64. Defaults to 4. Use `env()` for
passwords: it is resolved at compile time and does not stay written in the `.lum`.

---

### Querying

```lum
get endpoint("/articles"):
    return await postgres.query("select id, title from articles order by id")

get endpoint("/articles/:id", int id):
    List<Json> rows = await postgres.query("select title from articles where id = ?", id)
    if len(rows) == 0:
        return status(404)
    return rows[0]
```

`query()` returns `List<Json>`: a list of dictionaries, with the engine's types converted to
Lumen Script's —integer, decimal, boolean, string and `null`.

Binary columns —`BLOB` in sqlite and mysql— arrive in **base64**, not as a string. It is not
a preference: a blob is arbitrary bytes, and returning them as text left the response not
valid UTF-8, so the client receiving it failed rather than the request. In postgres a `bytea`
arrives in libpq's hex form (`\x68656c6c6f`).

```lum
post endpoint("/articles", Article a):
    int rows = await sqlite.exec(
        "insert into articles (title, views) values (?, ?)", a.title, a.views)
    int id = await sqlite.last_id()
    return { "id": id }.status(201)
```

`exec()` returns the number of affected rows. `last_id()` returns the last auto-generated id
**in sqlite and mysql**.

**Postgres does not have it**, and the module says so instead of making one up: there the id
is asked for in the query itself, which is more reliable anyway because it does not depend on
which connection served the insert.

```lum
post endpoint("/articles", Article a):
    List<Json> rows = await postgres.query(
        "insert into articles (title, views) values (?, ?) returning id",
        a.title, a.views)
    return rows[0].status(201)
```

**Parameters always travel separately, never concatenated.** Concatenating the query by hand
is the only way to open yourself to an injection, and the language does not make it easy.

The placeholder is **`?` in all three engines**. Postgres numbers its own —`$1`, `$2`…— but
its driver takes care of that, so the same query works on sqlite, mysql and postgres without
changing a letter:

```lum
await sqlite.query(  "select title from articles where id = ?", id)
await mysql.query(   "select title from articles where id = ?", id)
await postgres.query("select title from articles where id = ?", id)
```

Three details of the translation, which only matter in postgres:

- A query written with `$1` comes out untouched, so code written before this keeps working.
- A `?` inside a string, a quoted identifier, a comment or a `$$…$$` block is not a
  placeholder and is left alone.
- `?` is also postgres's JSONB operator —`data ? 'key'`. If the query carries **no
  parameters** nothing is translated and the operator works as is; if it does carry them,
  write `??` to say "this is the operator, not a placeholder".

Mixing `?` and `$1` in the same query is an error, because the numbering would clash.

### Transactions

```lum
post endpoint("/transfer"):
    await sqlite.begin()
    await sqlite.exec("update accounts set balance = balance - 30 where id = 1")
    await sqlite.exec("update accounts set balance = balance + 30 where id = 2")
    await sqlite.commit()
    return { "ok": true }
```

`begin()` pins the connection: everything that follows in that request goes through the same
one, and `commit()` or `rollback()` release it. If the handler ends —or blows up— with a
transaction open, Lumen issues a `ROLLBACK` and warns on the console. Without that, the next
request to take that connection from the pool would inherit the state.

### Errors

An engine error does not blow up the handler: it arrives as a dictionary with `error`.

```lum
get endpoint("/bad"):
    Json r = await sqlite.query("select * from does_not_exist")
    return r          # { "error": "no such table: does_not_exist" }
```

### Why it does not block

The sqlite, libpq and libmysqlclient clients are synchronous. Each module keeps a thread pool
with **one connection per worker**; `await` queues the work, releases the event loop and
picks it back up when the thread finishes. No `query()` stops the event loop, which is what
would make the whole efficiency argument collapse.

---

## 13. Server-Sent Events

```lum
sse endpoint("/metrics/:every", int every):
    int tick = 0
    sse.send("snapshot", "{\"startup\":true}")

    while sse.open:
        await sleep(every)
        tick = tick + 1
        sse.send("delta", "{\"tick\":" + str(tick) + "}", str(tick))
        if tick % 10 == 0:
            sse.ping("keepalive")
```

| Call | Frame |
|---|---|
| `sse.send(data)` | `data: ...` |
| `sse.send(event, data)` | `event: ...` + `data: ...` |
| `sse.send(event, data, id)` | adds `id:`, for reconnection with `Last-Event-ID` |
| `sse.ping(text)` | a `: ...` comment, ignored by the browser |
| `sse.open` | false when the connection closes |

The stream is opened before the handler runs and closed when it ends. There is no final
response to return.

---

## 14. WebSockets

```lum
ws endpoint("/echo") origins("https://myapp.com", "http://localhost:5173"):
    int n = 0
    ws.send("welcome")

    while ws.open:
        string msg = await ws.recv()
        if msg == null:
            break

        n = n + 1
        if msg == "bye":
            ws.send("closing after " + str(n) + " messages")
            ws.close()
            break

        ws.send("echo " + str(n) + ": " + msg)
```

`await ws.recv()` returns the message text, or `null` when the connection closes.

**`origins(...)` is required**, and leaving it out is a compile error. Browsers do not apply
the same-origin policy to the WebSocket handshake: without an allowlist, any site can open
the connection from your user's browser and inherit their cookies. An origin outside the list
gets a `403`.

---

## 15. Shared state

```lum
get endpoint("/visits"):
    return { "n": state.incr("visits") }

get endpoint("/counter"):
    return { "n": state.get("visits", 0) }
```

| | |
|---|---|
| `state.incr(key)` / `state.incr(key, n)` | Adds and returns the new value |
| `state.decr(key)` / `state.decr(key, n)` | Subtracts |
| `state.get(key)` / `state.get(key, default)` | Reads |
| `state.set(key, value)` | Writes |
| `state.remove(key)` | Deletes |

It is the **only** shared-state path between the event loops: each VM has its own stack and
heap and shares nothing. That is why it exposes operations and not properties —
`state.x = state.x + 1` would be a race between the read and the write.

It lives in process memory: it is lost on restart, and is not shared between machines.

---

## 16. File uploads

```lum
post endpoint("/avatar", File image):
    require image.content_type.starts_with("image/") else status(415)
    require image.size <= 5 * 1024 * 1024             else status(413)
    string name = image.save("./uploads")
    return { "url": "/static/uploads/" + name }

post endpoint("/gallery", List<File> photos):
    List<string> names = []
    for File f in photos:
        names.add(f.save("./uploads"))
    return { "names": names }
```

A `File` has `name`, `filename`, `content_type` and `size`, plus the method
`save(directory)`, which returns the name it was saved under.

`save()` keeps only the file component of the name: a `filename` with `..` or an absolute one
cannot escape the target directory.

With `File` (not `List<File>`), a missing file is a `422`. With `List<File>`, an empty list.

---

## 17. Error handlers

```lum
on error 404:
    return render("404.html", path=request.path, method=request.method)

on error 403:
    return { "error": "you cannot come in here", "code": error.code }

on error:
    log.error(error.message)
    return render("500.html")
```

In an `on error 422`, `error.messages` carries the complete list of validation messages —
empty if the 422 did not come from validating a body:

```lum
on error 422:
    return { "details": error.messages }
```

Without a code, it is the global handler. **It only covers 400–599**: with a 2xx the route's
handler has already written the response, and replacing it would be a response filter — that
is, middleware, which Lumen delegates to the proxy on purpose.

The status code is preserved. If the handler writes nothing, the default body is kept.

---

## 18. Functions

```lum
fn int double(int x):
    return x * 2

fn string greet(string name, string greeting = "hello"):
    return greeting + ", " + name

fn bool is_email(string s):
    return s.contains("@") and s.contains(".")
```

Declaration order does not matter: a function can call another declared further down, or in
another file. Recursion works, with a cap of 200 nested calls — going over gives a language
error, it does not exhaust the process memory.

Parameters accept default values, and the missing ones are filled in at the call site. A
parameter without a default cannot come after one that has one.

A function without `return` returns `null`. An error inside it **can be caught by the
caller**:

```lum
fn int divide(int a, int b):
    return a / b

get endpoint("/x"):
    try:
        return { "r": divide(1, 0) }
    catch e:
        return { "failure": e.message }
```

They can also be used inside a `validate:` block:

```lum
class Signup:
    string email

    validate:
        is_email(email)   "email: invalid format"
```

Declaring a function with a builtin's name is a compile error.

---

## 19. The language

### Types

```
int  long  float  double  bool  string        primitives, lowercase
Json  List<T>  Dict<K,V>  File                native classes, uppercase
```

`T?` marks that the value may be missing. Generics are **erased**: the checker verifies them
and they disappear before the bytecode. There are no user-defined generic classes.

### Multi-line strings

Three quotes, for SQL or HTML without fighting the line breaks:

```lum
get endpoint("/posts"):
    return await sqlite.query("""
        select id, title
        from posts
        order by date desc
        """)
```

The margin is not part of the string: it is the file's indentation, not the text's. A line
break right after the opening is removed, the closing line if it stands alone, and the
indentation **common** to the rest — which keeps the relative indentation between lines. The
above is exactly `select id, title\nfrom posts\norder by date desc`.

The escapes are the same as in a normal string: `\n`, `\t`, `\r`, `\0`, `\"`, `\\`.

### Truthiness

**False** are `null`, `false`, `0`, `0.0`, `""`, and the empty list and dictionary. It is
Python's rule.

It is not coercion: there is no type conversion inside the operators.

```lum
1 + "1"     # error
0 == "0"    # false
"n = " + str(n)     # this is how you concatenate a number
```

A trap inherited from Python: with an optional value, `if x:` does not tell "it is zero" from
"it did not arrive". For presence, use `x == null`.

### Statements

```lum
int n = 5
n = n + 1

if n > 3:
    ...
else if n > 1:
    ...
else:
    ...

while n > 0:
    n = n - 1

for int x in [1, 2, 3]:
    ...
for string k in myDictionary:      # walks the keys
    ...

require n > 0 else status(400)

try:
    int x = n / 0
catch e:
    log.warn(e.message)

break
continue
return value
```

`for` walks lists and a dictionary's keys. `try/catch` is resolved with a range table
computed at compile time, so a `return` or a `break` inside the `try` does not leave a
handler dangling. The error reaches the `catch` as a value with `message`.

### Expressions

Precedence, lowest to highest: `?:` · `or` · `and` · `not` · `==` `!=` · `<` `<=` `>` `>=` ·
`+` `-` · `*` `/` `%` · unary `-` · `.` `()` `[]`.

```lum
string role = age >= 18 ? "adult" : "minor"
```

---

## 20. Builtin reference

### Functions

| | |
|---|---|
| `text(v)` `html(v)` `json(v)` | Write the response |
| `render(template, k=v, ...)` | Renders a Lumen Script template |
| `status(code)` `redirect(target[, code])` `send_file(path)` | |
| `len(v)` | Size of a string, List or Dict |
| `str(v)` `int(v)` | Explicit conversion |
| `header(name[, default])` | Request header |
| `query(name[, default])` | Query parameter |
| `cookie(name[, default])` | Request cookie |
| `form(name[, default])` | Field of a `urlencoded` form |
| `await sleep(ms)` | Suspends |

### Reserved objects

| Object | Members | Where |
|---|---|---|
| `request` | `path` `method` `ip` | Any handler |
| `session` | any field, `clear()` | Any handler |
| `jwt` | `valid` `claims` | Any handler |
| `state` | `incr` `decr` `get` `set` `remove` | Any handler |
| `log` | `info` `warn` `error` | Everywhere |
| `sse` | `send` `ping` `open` | `sse` routes |
| `ws` | `send` `recv` `open` `close` | `ws` routes |
| `error` | `code` `message` `messages` | `on error` blocks |
| `sqlite` `postgres` `mysql` | `query` `exec` `begin` `commit` `rollback`; `last_id` in sqlite and mysql only | With `import` and its block in `app:` |

Using one outside its context is a compile error. Every method of a database module is
asynchronous: they are called with `await`.

### Methods by type

| Receiver | Methods |
|---|---|
| Any | `status(code)` `header(k, v)` `cookie(k, v, ...)` |
| `string` | `starts_with` `ends_with` `contains` `upper` `lower` `trim` |
| `List` | `add(v)` |
| `Dict` | `has(key)` `keys()` |
| `File` | `save(directory)` |

When the receiver's type is known at compile time —a declared parameter, a typed variable, a
literal— the name and the argument count are checked **there**, not at run time:

```
error: values of type string have no method 'mayusculas';
       it has status, header, cookie, starts_with, ends_with, contains, upper, lower, trim
```

The check continues down the chain, because every method knows what it returns:
`s.upper().recortar()` also fails at compile time. The same goes for a class's fields:
`p.noexiste` says which fields `p` has instead of silently returning `null`.

This reaches **inside the templates** too, because `render()` passes its argument types to
the template compiler: `{{ who.mayusculas() }}` is a `lumen --check` error, with the
template's file and line.

Where the type is not known —the variable of a `{% for %}`, a field of a `Json`— nothing is
checked and dispatch stays at run time, as before.

---

## 21. Common errors

| Message | What is happening |
|---|---|
| `the pattern declares ':id' but no parameter binds it` | The parameter is missing from the signature |
| `'sleep()' is asynchronous: you must write 'await sleep(...)'` | The `await` is missing |
| `'text()' is not asynchronous: the 'await' is unnecessary` | The `await` is redundant |
| `'sse' only exists inside an sse route` | A reserved object out of context |
| `a ws route needs origins(...)` | The origin allowlist is missing |
| `cannot add int and string` | An operation between different types |
| `the session is not configured` | `session: secret ...` is missing from `app:` |
| `'X' is not declared` | An unknown name, inside `validate` too |

They all come out with file, line, column and a cursor under the exact position.

---

## 22. How it works inside

```
lumen ./app  →  lex → parse → check → emit
                 ↓
              route table + bytecode   (once, not per request)
                 ↓
              N threads: event loop + its own VM, SO_REUSEPORT
```

**One compilation.** Lexer, parser, semantic analysis and emission happen at startup and once
per file change. Never per request.

**Two levels.** Declarative routes are entries in the radix tree with a native action: zero
interpreted steps. The rest run bytecode that only does glue — the real work (HTTP parsing,
routing, file I/O, templates, JSON) is always native C++.

**The VM does not know how to wait.** When it reaches an asynchronous call it gathers the
arguments and stops; the handler, which is already a coroutine, does the real `co_await` on
the engine and resumes it with the result. That is why the VM has its own stack and locals
instead of using C++'s: it is what allows stopping halfway.

**One VM per in-flight request**, held in the handler's coroutine frame. Chunks that cannot
suspend —and that is known at compile time— reuse one per thread and save the allocations.

**Reload.** The module has its own router; the engine only carries a wildcard entry that
delegates. Switching version is publishing a `shared_ptr`: no `dlopen`, no `.so`, no restart.
If the new version does not compile, it is not published.

**Step cap.** An infinite loop in a `.lum` is cut with an error instead of pinning an event
loop thread, which would take down every connection on that core. The counter resets on every
suspension, so a legitimate SSE loop can live for hours.
