# Bug hunting

> A log of the real failures that came out of running Lumen against its own engine —not
> against an instrumented version, not just compiling— and of how they were hunted down. It
> exists because the pattern repeats too often not to keep track: **compiling and linking
> proves nothing that matters.** A module that has not been run against its engine is not
> written, it is sketched.
>
> Format of every entry: what was suspected, how it was forced, what came out, the fix.
> Without that, "a bug was found" is not information — it is the easy part.

---

## Method

Ordered by what has produced results so far, not by elegance:

1. **Run against the real engine, not against a double.** The three database modules
   compiled and linked without a word for months; the four MySQL failures, the postgres one
   and the sqlite one only showed up when they were fired at a real server.
2. **Assert on the shape of the output, not just on the status code.** A `200` with a body
   no client can parse is worse than an error, because the failure shows up far from its
   origin. Checking "is this really valid JSON?" caught three separate bugs (postgres NaN,
   sqlite and mysql BLOBs, broken UTF-8) that a status-code-only test does not see.
3. **Sanitizers, not just functional tests.** TSan found the race on the *bind* buffers
   shared between pool workers (406 appearances in the report) and the one where shutdown
   touched other threads' loops (14 appearances) — neither of them showed up in a normal run.
4. **Read the code suspecting it, not trusting the comment next to it.** The multipart bug
   and the `env()` one came out of auditing code, not from something failing in production —
   and the first one had been documented as if it worked, in the project's own example.
5. **A systematic matrix of combinations**, instead of trusting that "there are already
   tests for that". Point 6 of this list.
6. **Fuzzing** — pending. Everything above is adversarial thinking driven by a person; what
   nobody thought to try, none of these methods finds.

---

## Log

### 2026-08-31 — postgres and mysql brought up to sqlite's level: what was missing and what did not apply

`sqlite` had three things the other two batteries did not: a valid-JSON check on the types
row, a case with a NUL byte inside a text, and a concurrency test that compares the
*content* of each response, not just the code. Before copying them over, each engine's
driver was read to find out which of the three really apply — copying a test that can never
fail is not coverage, it is noise.

**Applied:**

- **Concurrency with checked content, in both.** It is the most important of the three: it
  is exactly the shape the real shared *bind* buffer failure in MySQL had —one request
  answering with another's row— and the earlier test only looked at the `200` code. With the
  bug already fixed it was not going to fail today, but if someone regressed it by accident,
  the earlier test would never have noticed.
- **`json_valid` on `/types`, in both.** `sqlite` already had it; postgres and mysql did
  not, on exactly the route that carries each engine's binary type —`bytea`, `BLOB`— which
  is precisely the one that already broke the JSON once.
- **The NUL byte inside a text, in mysql.** `db_mysql.cpp` was read before writing anything:
  the value is built with `std::string(bufs[i].data(), std::min(lens[i], bufs[i].size()))`,
  which is already safe with NUL bytes in the middle — the 1023-byte truncation fix from this
  same session already covered this in passing. There was no failure to catch, but there was
  no test either to stop someone breaking it while touching that code without knowing why it
  matters.
- **The value of postgres's `bytea`, checked for the first time.** The column had been seeded
  in the schema since the battery existed, but nobody asserted anything about it — it came
  out in the `/types` JSON without any test looking at it. It comes out as postgres's own hex
  (`\x68656c6c6f`), not as base64: libpq already hands it over that way and the driver does
  not touch it, so it is JSON-safe by accident and not by design — the exact reason
  `README.md` already cites. Now there is a test asserting it instead of taking it as known.

**Discarded, with the reason checked rather than assumed:**

- **The prepared statement cache.** `sqlite` reuses a prepared statement between calls
  —hence its test that a `NULL` does not stay stuck from the previous turn— but neither
  `db_postgres.cpp` (`PQexecParams` directly, no `PQprepare`) nor `db_mysql.cpp`
  (`mysql_stmt_init` + `mysql_stmt_prepare` on every call) caches anything: there is no
  statement to reuse, so there is no risk to test.
- **`NaN`/`Infinity` in mysql.** Checked against a real server: `select 1e308 * 10` gives
  `ERROR 1690: DOUBLE value is out of range`. Under `STRICT_TRANS_TABLES` —the default mode—
  MySQL rejects the value before it comes into existence; the bug postgres does have has no
  equivalent here.

`mysql` goes from 26 to 29 tests; `postgres`, from 32 to 34. Project total: 246.

---

### 2026-08-31 — First fuzzing campaign: lexer/parser/checker, HTTP and multipart

Without libFuzzer —this toolchain is GCC, and `-fsanitize=fuzzer` is a clang builtin;
installing clang just for this would have been a new dependency for a problem that can be
solved without one. In its place, `fuzz/chaos.hpp`: "dumb" mutation with no coverage guidance
(bit flips, random bytes, insertions, deletions, splices) over real seeds, one child process
per case so that an ASan/UBSan `abort()` takes down only that case. Full detail in
[fuzz/README.md](fuzz/README.md).

Three targets, each with `lumen_script`/`lumen` rebuilt in full under
`-fsanitize=address,undefined` —not just the harness, or the sanitizer sees nothing of the
real code:

| Target | Cases | Failures |
|---|---:|---:|
| Lexer + parser + checker (`lumen_script::compile()`, the same path as `--check`) | 100,000 | 0 |
| HTTP parser (llhttp involved, `HttpParser::feed()`) | 100,000 | 0 |
| Multipart parser (`parse_multipart()`) | 100,000 | 0 |

**No crash, no hang, in 300,000 cases.** That is real news, not a placebo: the three targets
were genuinely unfuzzed before today, and the per-case `alarm()` would indeed have caught a
loop that never ends, not only a memory corruption.

**What this does NOT cover, so it is not read as more than it is:** without coverage
guidance, mutation tends to stay near the seeds —19 `.lum` files from `tests/cases` and half
a dozen hand-written HTTP requests— so code paths no seed touches are hardly visited. It also
tests nothing semantically *valid but wrong* —a case that compiles and should not, or that
compiles to the wrong thing— only what hangs or corrupts memory. And it does not fuzz the
three database modules or the template engine yet.

---

### 2026-08-31 — The parameter binding matrix enters the repo, with a negative control

A direct consequence of the multipart entry further down: the real bug is exactly what a
systematic matrix —origin (path / query / multipart) × type (scalar, `File`, `List<File>`) ×
presence (missing, mistyped, in two places at once)— would have found without anyone having
to suspect `save(dir)` first. `tests/cases/params.lum` and eleven new cases in
`tests/run_tests.sh` cover it: text next to a file, the four scalar types as a form field,
text next to a `List<File>`, the default value when the field is missing in both places, the
priority of the query over the form, and a mistyped scalar inside multipart.

**Negative control**, so that "the test passes" is not taken at face value: the binary was
rebuilt with the `project.cpp` from before the fix and only this section was run against it.
Exactly the six tests that touch the bug's pattern failed —text with a file, the three scalar
types, text with `List<File>`— and no others; the ones that do not depend on the new binding
(counting files, falling back to the default, query priority) kept passing, as they should.
The suite measures what it says it measures.

`regression` goes from 68 to 79 tests; the project total, from 230 to 241.

---

### 2026-08-31 — Text parameters never arrived in `multipart/form-data`

**Suspected** after auditing `save(dir)`: the directory argument is not sanitized, only the
uploaded file name, so if an app builds `dir` from a text parameter of the form itself
(`image.save("./uploads/" + album)`, the pattern the project itself documents), that
parameter could be a *path traversal* vector.

**Forced** by setting up that exact route and uploading a file with `curl -F`, varying the
order of the fields.

**What came out** was worse than what was being looked for: `album` arrived **always empty**,
with or without a traversal attempt, in any order. `prepare_args()` only looked at the
*query string* for a scalar parameter; never at the text parts of a multipart body. `form()`
was no rescue either — it only reads `application/x-www-form-urlencoded`. There was no way at
all to read a text field of a form with files.

**Fix:** before falling back to the default value, the multipart parts are searched for one
with that name and an empty `filename` (`src/lumen_script/project.cpp`, `prepare_args`). The
*query string* still takes priority if the name appears in both places.

```
POST /avatar  (multipart: image=<file>, title=empty)
→ before:  {"title":""}          # always, whatever happened
→ now:     422 "title: required" # if the handler validates it, now it can
```

---

### 2026-08-31 — `env("VAR")` on a non-existent variable, with no warning at all

**Suspected** that a deployment forgetting `SESSION_SECRET` would fail open: empty secret,
cookies signable by anyone.

**Forced** by compiling an `app:` with `session: secret env("DOES_NOT_EXIST")` and watching
what happened when using `session.*`.

**What came out** was that the specific fear was unfounded —`secret.empty()` is treated
exactly like "`session:` not configured at all", so it fails **closed**: every session
operation errors, `jwt.valid` is always `false`— but the real diagnosis stayed hidden: instead
of a clear error at startup, what appeared was *"the session is not configured"* on every
request, with no mention of the missing environment variable.

**Fix:** a warning on `stderr` with file:line:column at compile time
(`src/lumen_script/parser.cpp`), without blocking compilation —a `.lum` has no business
knowing the final deployment environment, and `--check` must be able to run without it.

```
lumen: warning: app.lum:4:20: the environment variable 'SESSION_SECRET'
is not defined; using "" instead
```

---

### 2026-08-31 — A body over 16 MB gave `400`, not `413`

**Found** while reading the HTTP parser: `kMaxBodySize` (16 MB, multipart uploads included)
existed and worked, but any parser limit violation —an oversized body included— was turned
into a generic `400 Bad Request`. Code 413 was already in `Response`'s reason table, unused
for this case.

**Fix:** the parser distinguishes the exact reason (`HttpParser::body_too_large()`); the
connection replies `413 Content Too Large` specifically for that case
(`src/http/http_parser.{hpp,cpp}`, `src/http/http_connection.cpp`).

---

### 2026-08-31 — The VM step cap does not see a loop that suspends without advancing

**Found** while reading the comment on the limit itself: it resets on every suspension, on
purpose, so a legitimate SSE can live for hours. But that leaves a gap the comment does not
cover: `while true: await sleep(0)` suspends and resumes without accumulating steps between
two suspensions, so the cap never sees it. A single client could pin a whole thread by
rescheduling a 0 ms timer without stopping.

**Fix:** a 1 ms floor on every `sleep()`, in the three places where the engine resumes a
handler —normal route, SSE, WS— (`src/lumen_script/project.cpp`, `clamp_sleep_ms`). It does
not prevent it entirely, but it bounds it to ~1000 resumptions/s per connection instead of as
many as the scheduler cares to give. Verified not to touch the legitimate case: `sleep(500)`
still takes exactly 500 ms; `sleep(0)` goes from 0 to ~1.3 ms.

---

### 2026-08-31 — The benchmark harness itself was measuring three servers at once

Not a Lumen bug — a measurement one. It is logged because the method that exposed it is the
same one as the rest of this list: not trusting that a process killed by `pkill` is really
dead.

**It came out while auditing** a `mixed with writes` run with 2,173 `500` responses that
matched nothing. It turned out to be `SO_REUSEPORT`: two binaries from an earlier A/B test
were still alive on the same port —58 minutes later— splitting connections with the one that
was supposedly being measured, without any visible error.

Second round: with `pkill` fixed, `database disk image is malformed` appeared. Lumen's
orderly shutdown **stops listening before it finishes draining**, and the harness restored
`tests-suite.db` as soon as the port was free, overwriting the file underneath a process that
still had it open.

**Fix (in the harness, outside the repo):** kill by *port*, not by process name — `pkill` did
not reach two binaries with different names serving the same thing — wait with `kill -0` for
the PID to really disappear, and restore the database with `rm` + an atomic `mv` instead of
`cp` in place.

---

### 2026-08-31 — MySQL: four failures that only showed up when it was run

It had compiled and linked for a long time; it had never been fired at a real server until
this round.

1. **Transactions did not open.** `BEGIN` is not preparable over MySQL's prepared statement
   protocol (error 1295); the error was silently discarded, so `ROLLBACK` returned success
   without having undone anything. Fix: `mysql_real_query` for statements with no parameters.
2. **Silent truncation past 1023 bytes.** `mysql_stmt_attr_set(STMT_ATTR_UPDATE_MAX_LENGTH)`
   + `mysql_stmt_store_result()` were missing before reading `field.max_length`.
3. **A large `BIGINT UNSIGNED` got pinned at `INT64_MAX`.** The unsigned value was not told
   apart from the signed one when converting.
4. **`bind` buffers shared between the N pool workers.** Only visible with ThreadSanitizer:
   406 appearances of the file in the race reports, zero after moving the *bind* state into
   locals per call.

---

### 2026-08-31 — Postgres: `NaN`/`Infinity` broke the outgoing JSON

**The same battery, run against postgres**, produced a failure that was not the module's but
the serializer's: postgres accepts `NaN` and `Infinity` in a `double precision` and they came
out written as such —`{"d":inf}`— which no JSON client can read. Fix: they are written as
`null`, just like `JSON.stringify`. The guide was corrected in passing too, as it promised a
`last_id()` postgres does not have — the driver already rejected it with a correct message;
it was the documentation that was lying.

---

### 2026-08-31 — sqlite and MySQL: a `BLOB` is not text

The third of the same family: a `BLOB` was returned as a string, so its raw bytes broke the
UTF-8 validity of the whole response. Fix: base64 in both engines. Postgres got away with it
by accident — libpq hands `bytea` over already in hex.

The three failures in this and the two previous entries share a pattern: **the module
answered `200` with a body no client can read.** The request does not fail, the receiver
does, far from the origin — which is why it is worth forcing on purpose instead of trusting
that "if it compiled and gave 200, it is fine".

---

### 2026-08-31 — Invalid UTF-8 reachable from the network by three routes

It was thought to be a rare sqlite case; it turned out to be reachable from the JSON body,
the *query* and the headers. A client sent a stray byte and Lumen answered `200` with a
response that same client could not read. Fix: a validation pass on the way out, separate
from the existing escaping loop, with the same 8-byte ASCII shortcut so the common case pays
nothing. Measured with three alternating repetitions: **+0.1% on a 57 KB JSON and −1.5% on
the 2 KB detail** — the first single-pass measurements said −5.9% / −10.1% and were
measurement noise, not the real cost.

---

### 2026-08-31 — The orderly shutdown touched other threads' loops

Found with ThreadSanitizer: 14 appearances of the file in the race reports during shutdown
with open connections. Fix: each thread stops accepting by looking only at its own state.

---

## Pending

- **Fuzz the database modules and the template engine.** Today's three targets do not cover
  them — see the campaign result, above.
- **More seeds for `fuzz_language`.** Without coverage guidance, mutation does not stray far
  from the 19 it has today (`tests/cases`); the complete programs in
  `LUMEN_SCRIPT-GRAMMAR.md` or other odd constructs would serve as extra seeds.
- **Repeat the campaign now and then, not once.** Without coverage there is no "it is already
  fuzzed" — every new run visits a different path by pure chance.
- **`numeric` precision in postgres.** `t_numeric` was stored as
  `12345678901234.1234567890` and came out `12345678901234.123`: OID 1700 is converted with
  `strtod`, which is `double`, not arbitrary precision. It is a known simplification —there is
  a `float`/`double` type in Lumen Script and no decimal one— not a conversion bug, but it is
  not documented as a limit anywhere either.
- **The documentation examples, compiled and run in CI.** The multipart bug had been
  documented as if it worked, in the project's own example — nobody had recompiled it since
  it was written.
