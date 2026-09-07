# Lumen

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat&logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C?style=flat&logo=cmake&logoColor=white)
![Linux only](https://img.shields.io/badge/platform-Linux-FCC624?style=flat&logo=linux&logoColor=black)
![Binary size](https://img.shields.io/badge/binary-1.5_MB-informational?style=flat)
![Tests](https://img.shields.io/badge/tests-246_passing-brightgreen?style=flat)

Lumen is a web framework with its own language built in. Routes, models, and validation are
written in **Lumen Script**, a small statically-typed language, and the `lumen` binary
compiles the project to bytecode and serves it — no separate compiler, no build step, nothing
sitting between your code and the framework.

```lum
app:
    name      "My blog"
    port      8080
    templates "./templates"

    static "/static" -> "./public"
    docs

    session:
        secret env("SESSION_SECRET")


class Article:
    int     id
    string  title
    string  body
    string? tags

    validate:
        title != ""       "title: required"
        len(body) >= 20    "body: minimum 20 characters"


get endpoint("/"):
    return render("index.html")

get endpoint("/articles/:id", int id):
    return { "id": id, "title": "Hello world" }

post endpoint("/articles", Article a):
    # If the body fails to parse or fails validation, the response is 422
    # and this line never runs.
    return { "created": a.title }.status(201)


group("/admin"):
    require session.role == "admin" else redirect("/login")

    get endpoint("/panel"):
        return render("panel.html", user=session.user)
```

```
$ lumen ./my-blog
lumen: 3 file(s), 5 route(s) — 2 declarative, 3 with logic
Lumen running on http://0.0.0.0:8080 (threads=16, press CTRL+C to quit)
```

Save the file and it reloads. No recompiling, no restarting, no CMake.

---

## Architecture

```
lumen ./app          →  lex → parse → check → emit
                       ↓
                    route table + bytecode  (built ONCE, not per request)
                       ↓
                    N threads, each with its own event loop + VM, SO_REUSEPORT
```

One event loop per core, `SO_REUSEPORT`, no GIL, no global GC. HTTP parsing is llhttp; JSON
and static files (`sendfile(2)`) are handled directly, nothing borrowed from a scripting
runtime.

A route that resolves entirely at compile time — `return render("index.html")` — becomes a
native action and runs zero bytecode. The binary reports how many routes take each path on
startup.

Every request gets its own VM, isolated in the handler's coroutine frame. Nothing to lock,
nothing to synchronize between cores; shared state is opt-in and atomic.

There's no implicit type coercion anywhere in the language. `1 + "1"` is a compile error, not
`"11"`. It's the one rule that would have saved a generation of developers from learning to
write `===` out of self-defense.

---

## Language

Indentation like Python. Static types like C++. Routes like Flask. Request binding like
FastAPI.

### Parameters

```lum
get endpoint("/users/:id", int id, int page = 1, string q):
    return { "id": id, "page": page, "searching": q }
```

`:id` binds to the path segment; `page` and `q` bind to the query string, with a default
where one is given. The compiler checks that every `:name` in the pattern has a parameter to
receive it, and vice versa — something Flask can't do, because its types live inside a
string.

A parameter typed as a class binds to the request **body**, with validation and 422 handled
for you. `File` or `List<File>` binds to multipart parts.

### Responses

| | |
|---|---|
| `return { "a": 1 }` | 200, JSON |
| `return render("x.html", k=v)` | HTML through Lumen Script templates |
| `return text("hi")` / `html(...)` | plain text / HTML |
| `return send_file(path)` | file, `sendfile(2)` |
| `return redirect("/other")` | 302 |
| `return status(204)` | status code, no body |

No mutable `response` object to carry through the handler.

### Guards

```lum
group("/api/v1"):
    require jwt.valid else status(401)

    get endpoint("/me"):
        return { "sub": jwt.claims["sub"] }
```

CORS, compression, and rate limiting live on the proxy, so the only middleware left standing
was route protection. That's `if not X: return Y` with better manners, not a concept of its
own. Groups nest and guards stack.

### Reserved objects

| Object | Where | Gives |
|---|---|---|
| `request` | any handler | `path`, `method`, `ip` |
| `session` | any handler | signed cookie, any field |
| `jwt` | any handler | `valid`, `claims` |
| `state` | any handler | store shared across threads |
| `log` | everywhere | `info`, `warn`, `error` |
| `sse` | `sse` routes | `send`, `ping`, `open` |
| `ws` | `ws` routes | `send`, `recv`, `open`, `close` |
| `error` | `on error` blocks | `code`, `message` |

Use `sse` in a `get` route, or `error` outside `on error`, and it won't compile. Get a
method or field wrong on a known type — `name.uppercase()` on a `string`, `p.missing` on a
class — same story, and that reaches into templates too.

---

## Design decisions

| Topic | Decision |
|---|---|
| Auth | Vendored HMAC-SHA256, HS256. No OpenSSL. RS256 is not available |
| Real time | SSE and WebSockets |
| Transport | Plain HTTP/1.1, TLS and HTTP/2 belong to the reverse proxy |
| Execution | Bytecode on a custom VM, one VM per event-loop thread |
| Compilation | Built into the binary. No external toolchain, no transpilation to C++ |
| Persistence | `sqlite`, `postgres`, and `mysql` modules over a thread pool and `await`. `?` placeholder in all three — the postgres driver translates it to `$1` |
| Templates | Custom engine, shaped like Jinja2, with Lumen Script expressions inside |
| Types | `class` for known, validated shape; `Json` for dynamic data; containers for homogeneous data — no `Any`, which would poison the whole type system |
| Generics | Native containers only, erased at compile time. No user-defined generic classes |
| Config | Lumen Script, `app:` block, once per project. No YAML or TOML — one language to learn instead of two, and a misspelled port is a compile error |
| Layout | `lumen ./my-app` reads the tree recursively. Order does not matter; compilation happens in two passes |

### Templates

We tried an off-the-shelf engine first. It worked, and it also dragged in Boost, fmt,
rapidjson, and a `build/_deps` directory pushing 800 MB just to render a string. We wrote our
own instead: same shape as Jinja2 — `{{ }}`, `{% if %}`, `{% for %}`, `{% extends %}`,
`|safe` — except what's inside the braces is Lumen Script, checked by the same compiler as
everything else. A template typo is a `lumen --check` error with a file and a line, not a
2 a.m. page.

| | Off-the-shelf engine | Our engine |
|---|---|---|
| Binary | 7,951,752 B | **1,592,032 B** |
| `build/_deps` | ~800 MB | **doesn't exist** |
| Build from scratch | minutes | **19 s** |
| Network on first `cmake` | required | **none** |

We gave up `{{ super() }}` and Jinja2's filters (`|upper`, `|join`...) — those are just Lumen
Script methods now.

---

## Examples

### Login with session and role

```lum
class Login:
    string username
    string password

    validate:
        username != ""  "username: required"
        password != ""  "password: required"

post endpoint("/login", Login data):
    if data.password != "hunter2":
        return status(401)

    session.user = data.username
    session.role = data.username == "alice" ? "admin" : "user"
    return redirect("/")

post endpoint("/logout"):
    session.clear()
    return redirect("/")

group("/admin"):
    require session.role == "admin" else status(403)

    get endpoint("/panel"):
        return render("panel.html", of=session.user)
```

`session` is a cookie signed with HMAC-SHA256, no server-side state, always `HttpOnly`. An
invalid signature clears it — never leaves it half-populated.

### Real time

```lum
sse endpoint("/metrics"):
    int tick = 0
    while sse.open:
        await sleep(2000)
        tick = tick + 1
        sse.send("delta", "{\"tick\":" + str(tick) + "}", str(tick))

ws endpoint("/chat") origins("https://myapp.com"):
    ws.send("welcome")
    while ws.open:
        string msg = await ws.recv()
        if msg == null:
            break
        ws.send("echo: " + msg)
```

`await` suspends the handler without blocking the event loop — eight concurrent 500 ms
requests take 500 ms, not four seconds.

`origins(...)` is required on a `ws` route. Skip it and it won't compile, because without an
allowlist any site can open the connection from your user's browser.

### File uploads

```lum
post endpoint("/avatar", File image):
    require image.content_type.starts_with("image/") else status(415)
    require image.size <= 5 * 1024 * 1024              else status(413)
    return { "saved": image.save("./uploads") }

post endpoint("/gallery", List<File> photos):
    List<string> names = []
    for File f in photos:
        names.add(f.save("./uploads"))
    return { "names": names }
```

`save()` keeps only the file name — a `filename` carrying `..` or an absolute path can't
escape the target directory.

### Shared state

```lum
get endpoint("/visits"):
    return { "n": state.incr("visits") }
```

`state` is the one shared-state path across the N event loops, and it exposes operations
instead of properties on purpose: `state.x = state.x + 1` would race the read against the
write.

### Custom error pages

```lum
on error 404:
    return render("404.html", path=request.path)

on error:
    log.error(error.message)
    return render("500.html")
```

---

## Errors

```
./app.lum:12:19: error: pattern declares ':id' but no parameter binds it
  12 | get endpoint("/users/:id"):
     |              ^
```

File, line, column, cursor — every time. And the compiler catches more than you'd expect: a
field that doesn't exist in a `validate` rule, a missing or extra `await`, `sse` on the
wrong route type, a duplicate class, two bodies on one route, a `ws` without `origins`.

If the file you save doesn't compile, the previous version keeps serving. A typo doesn't
take the server down.

---

## Features

| | |
|---|---|
| **Routing** | Radix tree, `:param`, `{param}`, `*`, nested groups |
| **Input** | Path, query with defaults, typed JSON body, multipart, headers, cookies, forms |
| **Validation** | Per-class `validate:` block → automatic 422 with every message |
| **Output** | JSON, HTML, text, custom templates with `{% extends %}`, files, redirects, status codes |
| **Async** | `await sleep(ms)`, `await ws.recv()`, cancellation on disconnect |
| **Real time** | SSE with `id:` for reconnection, RFC 6455 WebSockets |
| **Auth** | `session` in a signed cookie, JWT HS256 with `alg`, `exp`, and `iss` verification |
| **State** | Shared store with atomic operations |
| **Persistence** | `sqlite`, `postgres`, and `mysql` modules: connection pool, transactions, parameterized queries |
| **Files** | MIME, ETag, 304, `sendfile(2)`, SPA support, dotfile and path-traversal blocking |
| **Language** | Classes, `for`, `while`, `try/catch`, ternary, lists, dicts, methods |
| **Docs** | `/openapi.json` and `/docs` generated from the AST |
| **Observability** | Logger with rotation, `/health`, `/metrics` in Prometheus format |
| **Reload** | File watching and atomic module swap |

**Not included:** TLS and HTTP/2, CORS, compression, rate limiting, and security headers —
that's the reverse proxy's job. Also no user-defined generic classes: `List<T>` and
`Dict<K,V>` exist, `class Box<T>` doesn't.

---

## Build

Requires **Linux** (epoll, `sendfile(2)`, `SO_REUSEPORT`), **CMake 3.20+**, and **C++20**
(GCC 11+ or Clang 13+). No OpenSSL, no zlib.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Nothing gets downloaded. The only vendored dependency is **llhttp** — 360 KB of
dependency-free C. JSON, templates, and cryptography are built in-house: no `node_modules`,
no lockfile tracking four thousand packages you've never heard of, no install step that
needs a network connection before it does anything at all. The whole tree builds from
scratch in about twenty seconds, and the binary comes out at **1.5 MB**.

The only system dependencies are the database clients you actually want — each module
builds only if its client is present: `libsqlite3-dev`, `libpq-dev`, `libmysqlclient-dev`.

jemalloc is optional but worth it: with several event loops and a database pool, glibc's
`malloc` serializes on its arenas and becomes the bottleneck. `cmake` links it automatically
if it finds it; otherwise the build proceeds and says so.

```bash
sudo apt install libjemalloc-dev     # optional
```

```bash
lumen ./my-app          # every .lum file in the directory, recursively
lumen app.lum            # just that file
lumen a.lum b.lum        # just those
lumen ./app --check      # compile and exit
lumen ./app --no-watch   # no hot reload
lumen ./app --verbose    # one log line per request
lumen ./app --autotest   # walks the endpoints after startup and after each reload
```

---

## Testing

**246 tests** across six suites, covered in detail in
[LUMEN_SCRIPT-GRAMMAR.md](LUMEN_SCRIPT-GRAMMAR.md): 79 regression tests that drive the
binary over the socket the way it's actually used, including the parameter-binding matrix in
[bug-hunting.md](bug-hunting.md); 48 for the template engine; 19 for placeholder
translation; and 37 + 29 + 34 for the `sqlite`, `mysql`, and `postgres` modules against real
database engines.

```bash
cmake --build build --target lumen-bin && ctest --test-dir build
```

The three database modules pass under AddressSanitizer with leak detection and under
ThreadSanitizer with 60 concurrent clients. Against a real PostgreSQL 18, with `pool 4` and
1 s queries, the pool saturates at its own size and queues the rest without dropping a
single request:

| Concurrent | Time |
|---|---|
| 1 | 1016 ms |
| 2 | 1015 ms |
| 4 | 1039 ms |
| 8 | 2015 ms (two batches) |
| 16 | 4018 ms (four batches) |

A route that doesn't touch the database still answers in 10 ms while that pool is fully
saturated by other requests.

Bugs found by testing against the real engine, not just compiling, are logged in
[bug-hunting.md](bug-hunting.md). The formal grammar, with a full manual, is in
[LUMEN_SCRIPT-GRAMMAR.md](LUMEN_SCRIPT-GRAMMAR.md).
