#!/usr/bin/env bash
#
# Regression suite for Lumen.
#
# Runs the binary against real .lum files and checks the responses.
# It is written in shell on purpose: it tests the binary the way it is used, over
# the socket, without linking anything from the project.
#
#   tests/run_tests.sh [path-to-binary]
#
# Exits with 0 if everything passes.

set -u

LUMEN="${1:-$HOME/lumen-build/lumen}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
PORT=${LUMEN_TEST_PORT:-8790}
SRV=""

passed=0
failed=0

# ─── Helpers ─────────────────────────────────────────────────────────────────

red()  { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }

ok()   { passed=$((passed + 1)); printf '  ok    %s\n' "$1"; }
fail() {
    failed=$((failed + 1))
    red "  FAIL $1"
    printf '        expected: %s\n        got: %s\n' "$2" "$3"
}

# Starts a server with the given .lum and waits for it to answer.
# Each suite uses its own port: with SO_REUSEPORT two processes share a
# port and the kernel splits connections between them, which would skew everything.
start_server() {
    stop_server
    PORT=$((PORT + 1))
    "$LUMEN" --no-watch --port "$PORT" "$1" > "$TMP/srv.log" 2>&1 &
    SRV=$!
    for _ in $(seq 1 60); do
        if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/__ping__" 2>/dev/null; then
            return 0
        fi
        kill -0 "$SRV" 2>/dev/null || { red "the server died on startup:"; cat "$TMP/srv.log"; return 1; }
        sleep 0.2
    done
    red "the server did not answer"; cat "$TMP/srv.log"; return 1
}

stop_server() {
    [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null
    # Wait ONLY for that pid: a bare `wait` would also wait for the
    # server and hang the suite.
    wait "$SRV" 2>/dev/null
    SRV=""
}

# check <name> <method> <path> <expected-code> [expected-substring] [data]
check() {
    local name="$1" method="$2" path="$3" want_code="$4" needle="${5:-}" data="${6:-}"
    local args=(-sS --max-time 10 -X "$method" -o "$TMP/body" -w '%{http_code}')
    [ -n "$data" ] && args+=(-H 'Content-Type: application/json' -d "$data")

    local got
    got=$(curl "${args[@]}" "http://127.0.0.1:$PORT$path" 2>/dev/null)
    local body
    body=$(cat "$TMP/body" 2>/dev/null)

    if [ "$got" != "$want_code" ]; then
        fail "$name" "code $want_code" "code $got — $body"
        return
    fi
    if [ -n "$needle" ] && ! grep -qF "$needle" "$TMP/body"; then
        fail "$name" "to contain '$needle'" "$body"
        return
    fi
    ok "$name"
}

# check_mp <name> <path> <expected-code> <expected-substring> -- <curl -F flags...>
# Separate from check(): a multipart body is not a string you can pass
# with -d, it is named parts, and curl builds them with one -F per field.
check_mp() {
    local name="$1" path="$2" want_code="$3" needle="${4:-}"; shift 4
    local got
    got=$(curl -sS --max-time 10 -X POST -o "$TMP/body" -w '%{http_code}' "$@" \
          "http://127.0.0.1:$PORT$path" 2>/dev/null)
    local body
    body=$(cat "$TMP/body" 2>/dev/null)

    if [ "$got" != "$want_code" ]; then
        fail "$name" "code $want_code" "code $got — $body"
        return
    fi
    if [ -n "$needle" ] && ! grep -qF "$needle" "$TMP/body"; then
        fail "$name" "to contain '$needle'" "$body"
        return
    fi
    ok "$name"
}

# fails_to_compile <name> <file> <error-substring>
fails_to_compile() {
    local name="$1" fich="$2" needle="$3"
    local out
    out=$("$LUMEN" --check "$fich" 2>&1)
    if [ $? -eq 0 ]; then
        fail "$name" "a compile error" "it compiled without complaining"
        return
    fi
    if ! printf '%s' "$out" | grep -qF "$needle"; then
        fail "$name" "an error containing '$needle'" "$(printf '%s' "$out" | head -2)"
        return
    fi
    ok "$name"
}

compiles() {
    local name="$1" fich="$2"
    if "$LUMEN" --check "$fich" > "$TMP/check" 2>&1; then
        ok "$name"
    else
        fail "$name" "it to compile" "$(head -3 "$TMP/check")"
    fi
}

cleanup() { stop_server; rm -rf "$TMP"; }
trap cleanup EXIT

# ─── Suites ──────────────────────────────────────────────────────────────────

echo "== language =="
start_server "$HERE/cases/language.lum" || exit 1
check "arithmetic"          GET /arithmetic       200 '"sum":7'
check "strings"             GET /strings          200 '"upper":"HELLO"'
check "multiline without margin" GET /multiline     200 '"sql":"SELECT id\nFROM posts"'
check "one-line multiline"   GET /multiline       200 '"loose":"on one line"'
check "multiline escapes"  GET /multiline       200 '"escapes":"with \"quotes\""'
check "truthiness"           GET /truthiness        200 '"zero":false'

# JSON has to be UTF-8 (RFC 8259).  A stray 0xFF arrives from the network by
# several routes, and without sanitizing it produced a response that not even
# the client that sent it could parse: the request does not fail, the receiver does.
json_utf8() {
    local name="$1"; shift
    curl -sS --max-time 10 -o "$TMP/body" "$@" 2>/dev/null
    if python3 -c "import json,sys; json.load(open(sys.argv[1], encoding='utf-8'))"             "$TMP/body" 2>/dev/null; then
        ok "$name"
    else
        fail "$name" "valid UTF-8 JSON" "$(head -c 120 "$TMP/body" | cat -v)"
    fi
}
json_utf8 "broken utf8 in the query"    "http://127.0.0.1:$PORT/echo_query?q=a%FFb"
json_utf8 "broken utf8 in the header" -H "$(printf 'X-Test: aÿb')"           "http://127.0.0.1:$PORT/echo_header"
# And what IS valid has to come out UNTOUCHED: sanitizing cannot spoil good
# text, which is the half that really matters.
check "valid utf8 untouched" GET /echo_valid 200 '"echo":"añoñó 🐻 ñ"'
check "no coercion"        GET /coercion         500 'cannot add'
check "elif conditional"    GET /classify/0      200 '"r":"zero"'
check "else conditional"    GET /classify/99     200 '"r":"big"'
check "while loop"         GET /sum_up_to/10    200 '"total":55'
check "for loop and break"   GET /evens            200 '"evens":[2,4,6]'
check "operators"          GET /operators       200 '"a":2'
check "pre-increment"   GET /increment       200 '"pre":7'
check "indices"             GET /indices          200 '"l":[99,20,25]'
check "ternary"            GET /ternary/20      200 '"role":"adult"'
check "try catches"         GET /captura          200 'division by zero'
check "uncaught error"  GET /boom         500 'division by zero'

echo "== routes and parameters =="
start_server "$HERE/cases/routes.lum" || exit 1
check "path parameter"   GET /echo/42           200 '"id":42'
check "query with default"   GET /pagina           200 '"page":1'
check "query explicita"     GET '/pagina?page=7'  200 '"page":7'
check "invalid type"       GET /echo/abc          400 'invalid parameter'
check "path inexistente"    GET /nada             404 'not found'
check "group with prefix"   GET /api/v1/hello      200 '"v":1'
check "guard denies"      GET /admin/panel      403
check "guard allows"      GET '/admin/panel?k=abre' 200 '"panel":true'
check "404 handler"       GET /tampoco          404 '"path":"/tampoco"'

# ── Parameter binding matrix ──────────────────────────────────────────────
# Origen (path / query / multipart) x type (escalar, File, List<File>) x
# presence (missing, mistyped, in two places at once).  See cases/params.lum:
# the real bug was a text parameter ALWAYS empty on a route with files,
# and it only shows up when the two live on the same route -- testing query and
# multipart separately, as the rest of the suite did, never caught it.
echo "== parameter binding =="
start_server "$HERE/cases/params.lum" || exit 1

check "missing query without a default gives the type's zero" GET /query 200 '"q":""'

check_mp "multipart: text next to a file" /mp/one 200 '"title":"hello"' \
    -F "f=@$HERE/cases/params.lum;filename=a.txt" -F "title=hello"
check_mp "multipart: the file arrives too" /mp/one 200 '"filename":"a.txt"' \
    -F "f=@$HERE/cases/params.lum;filename=a.txt" -F "title=hello"

check_mp "multipart: string"  /mp/types 200 '"s":"hello"' \
    -F "f=@$HERE/cases/params.lum;filename=a.txt" -F "s=hello" -F "n=7" -F "b=true"
check_mp "multipart: int"     /mp/types 200 '"n":7' \
    -F "f=@$HERE/cases/params.lum;filename=a.txt" -F "s=hello" -F "n=7" -F "b=true"
check_mp "multipart: bool"    /mp/types 200 '"b":true' \
    -F "f=@$HERE/cases/params.lum;filename=a.txt" -F "s=hello" -F "n=7" -F "b=true"

check_mp "multipart: text next to a List<File>" /mp/list 200 '"album":"holidays"' \
    -F "fs=@$HERE/cases/params.lum;filename=a.txt" -F "album=holidays"
check_mp "multipart: counts the files in the list" /mp/list 200 '"n":2' \
    -F "fs=@$HERE/cases/params.lum;filename=a.txt" \
    -F "fs=@$HERE/cases/params.lum;filename=b.txt" -F "album=x"

check_mp "multipart: missing field falls back to the default" /mp/default 200 '"label":"no-label"' \
    -F "f=@$HERE/cases/params.lum;filename=a.txt"

check_mp "multipart: the query beats the form field" "/mp/prioridad?origin=query" 200 '"origin":"query"' \
    -F "f=@$HERE/cases/params.lum;filename=a.txt" -F "origin=formulario"

check_mp "multipart: mistyped scalar gives 400" /mp/bad 400 'invalid parameter' \
    -F "f=@$HERE/cases/params.lum;filename=a.txt" -F "n=no-es-un-number"

echo "== classes and validation =="
start_server "$HERE/cases/classes.lum" || exit 1
check "valid body"       POST /add 201 '"creado":"Ana"' '{"name":"Ana","age":30}'
check "field required"   POST /add 422 'age: required' '{"name":"Ana"}'
check "wrong type"     POST /add 422 'expected int' '{"name":"Ana","age":"30"}'
check "broken rule"    POST /add 422 'must be of age' '{"name":"Ana","age":10}'
check "every message"  POST /add 422 'name: required' '{"name":"","age":10}'
check "invalid json"       POST /add 400 'invalid JSON' '{roto'
check "messages in on error" POST /add 422 '"count":2' '{"name":"","age":10}'
check "constructor"         GET /punto/3/4        200 '"cuadrado":25'
check "method with a default"  GET /label/1/2     200 '"other":"Q(1,2)"'
check "user function"  GET /doble/21         200 '"r":42'
check "recursion"           GET /factorial/5      200 '"r":120'
check "recursion cap"   GET /infinita         500 'too much recursion'

echo "== session and jwt =="
start_server "$HERE/cases/session.lum" || exit 1
check "no session"          GET /quien            200 '"user":null'
check "protected area"      GET /admin/panel      403
check "forged cookie"  GET /quien            200 '"user":null'
check "jwt missing"         GET /api/yo           401
check "invalid jwt"        GET /api/yo           401

echo "== database =="
rm -f "$TMP/tests.db"
start_server "$HERE/cases/data.lum" || exit 1
check "create table"         GET  /create           200 '"ok":true'
check "insert"            POST /add/ana        201 '"id":1'
check "insert another"       POST /add/bob        201 '"id":2'
check "list"              GET  /all           200 '"name":"ana"'
check "lookup by id"       GET  /one/1           200 '"name":"ana"'
check "no encontrado"       GET  /one/99          404
check "sql injection"       GET  "/search?q=ana'%20OR%20'1'='1" 200 '"encontrados":0'
check "transaction"         POST /transfer      200 '"ok":true'
check "balances after commit"  GET  /balances          200 '"balance":70'
check "rollback"            POST /undo         200 '"deshecho":true'
check "balances after rollback" GET /balances          200 '"balance":70'
check "engine error"     GET  /bad            200 'no such table'

echo "== compile errors =="
compiles    "the repo examples compile" "$HERE/cases/language.lum"
fails_to_compile "pattern without a parameter"  "$HERE/cases/bad/pattern.lum"   "no parameter binds it"
fails_to_compile "missing await"       "$HERE/cases/bad/await.lum"    "is asynchronous"
fails_to_compile "object out of place" "$HERE/cases/bad/sse.lum"      "only exists inside a route sse"
fails_to_compile "ws without origins"        "$HERE/cases/bad/ws.lum"       "needs origins"
fails_to_compile "unknown field"     "$HERE/cases/bad/validate.lum" "is not declared"
fails_to_compile "unknown method"    "$HERE/cases/bad/method.lum"   "has no method"
fails_to_compile "method on a string"   "$HERE/cases/bad/method_type.lum" "have no method"
fails_to_compile "field of a class"    "$HERE/cases/bad/field_type.lum"  "has no field"
fails_to_compile "module not imported"   "$HERE/cases/bad/import.lum"   "missing 'import sqlite'"
# Expression types are checked at RUN TIME: the compiler
# it verifies names, arity, context, and the methods and fields of a receiver
# whose type it knows -- but not that `s - 1` adds up.
# That is already covered by the "no coercion" test of the language suite.

# ─── Summary ─────────────────────────────────────────────────────────────────

echo
if [ "$failed" -eq 0 ]; then
    green "$passed tests, all passing"
    exit 0
fi
red "$passed passed, $failed failed"
exit 1
