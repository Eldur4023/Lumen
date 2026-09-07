#!/usr/bin/env bash
#
# Suite for Lumen's postgres module.
#
# It is separate from run_tests.sh because it needs a server: without one, this suite
# it SKIPS itself instead of failing, so `ctest` stays green on a machine
# with no postgres.  Exit code 77 is the one CMake understands as
# "omitida".
#
# To set the environment up once.  Each statement in its own invocation:
# `psql -c` with several wraps them in a transaction, and CREATE DATABASE cannot
# run inside one.
#
#   sudo apt install -y postgresql
#   sudo service postgresql start
#   sudo -u postgres psql -c "CREATE USER lumen WITH PASSWORD 'lumen';"
#   sudo -u postgres psql -c "CREATE DATABASE lumen_tests OWNER lumen;"
#
#   tests/run_postgres.sh [path-to-binary]
#
# The credentials are written in on purpose: it is a test database recreated
# from scratch on every pass, just like the .db file of the sqlite suite.

set -u

LUMEN="${1:-$HOME/lumen-build/lumen}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
PORT=${LUMEN_TEST_PG_PORT:-8810}
SRV=""

PGURL="postgresql://lumen:lumen@127.0.0.1/lumen_tests"

passed=0
failed=0

red()  { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
grey()  { printf '\033[90m%s\033[0m\n' "$*"; }

ok()   { passed=$((passed + 1)); printf '  ok    %s\n' "$1"; }
fail() {
    failed=$((failed + 1))
    red "  FAIL $1"
    printf '        expected: %s\n        got: %s\n' "$2" "$3"
}

stop_server() {
    [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null
    # Only that pid: a bare `wait` would also wait for the server and
    # would hang the whole suite.
    wait "$SRV" 2>/dev/null
    SRV=""
}
trap 'stop_server; rm -rf "$TMP"' EXIT

# ─── Can it run? ─────────────────────────────────────────────────────────────

if ! command -v psql > /dev/null 2>&1; then
    grey "postgres: no client installed — suite skipped"
    exit 77
fi
if ! psql "$PGURL" -c "select 1" > /dev/null 2>&1; then
    grey "postgres: cannot connect to lumen_tests — suite skipped"
    grey "          (the instructions for setting it up are in this file's header)"
    exit 77
fi
if ! "$LUMEN" --check "$HERE/cases/postgres.lum" > "$TMP/check" 2>&1; then
    if grep -q "postgres" "$TMP/check" && grep -q "import" "$TMP/check"; then
        grey "postgres: the binary was built without the module — suite skipped"
        exit 77
    fi
    red "the test file does not compile:"; cat "$TMP/check"; exit 1
fi

# ─── Helpers ─────────────────────────────────────────────────────────────────

check() {
    local name="$1" method="$2" path="$3" want_code="$4" needle="${5:-}"
    local got
    got=$(curl -sS --max-time 10 -X "$method" -o "$TMP/body" -w '%{http_code}' \
          "http://127.0.0.1:$PORT$path" 2>/dev/null)
    local body; body=$(head -c 400 "$TMP/body" 2>/dev/null)
    if [ "$got" != "$want_code" ]; then
        fail "$name" "code $want_code" "code $got — $body"; return
    fi
    if [ -n "$needle" ] && ! grep -qF "$needle" "$TMP/body"; then
        fail "$name" "to contain '$needle'" "$body"; return
    fi
    ok "$name"
}

# Checks that the response body is really valid JSON.  A NaN or an
# Infinity from postgres would come out as `nan` or `inf`, which no client can read.
json_valid() {
    local name="$1" path="$2"
    curl -sS --max-time 10 -o "$TMP/body" "http://127.0.0.1:$PORT$path" 2>/dev/null
    if python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$TMP/body" 2>/dev/null; then
        ok "$name"
    else
        fail "$name" "JSON valido" "$(head -c 200 "$TMP/body")"
    fi
}

# ─── Arranque ────────────────────────────────────────────────────────────────

psql "$PGURL" -q -v ON_ERROR_STOP=1 -f "$HERE/cases/postgres-schema.sql" > /dev/null 2>&1 || {
    red "cannot load the schema:"
    psql "$PGURL" -v ON_ERROR_STOP=1 -f "$HERE/cases/postgres-schema.sql" 2>&1 | tail -5
    exit 1; }

echo "== arranque =="
"$LUMEN" --no-watch --port "$PORT" "$HERE/cases/postgres.lum" > "$TMP/srv.log" 2>&1 &
SRV=$!
for _ in $(seq 1 60); do
    curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/__ping__" 2>/dev/null && break
    kill -0 "$SRV" 2>/dev/null || { red "the server died on startup:"; cat "$TMP/srv.log"; exit 1; }
    sleep 0.3
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/__ping__" || {
    red "the server did not answer"; cat "$TMP/srv.log"; exit 1; }
ok "starts and connects"

echo "== lectura =="
check "select without parameters" GET /all      200 '"title":"length"'
check "select with a parameter"  GET /one/1      200 '"author":"Ana"'
check "missing row"    GET /one/99999  404

# The Lumen Script placeholder is `?` and the driver translates it to $1: here it is checked
# against a real server, not just against the function on its own.
echo "== placeholders =="
check "dos placeholders en orden" GET '/two?author=Ana&views=0' 200 '"title":"length"'
check "estilo \$1 sigue valiendo" GET /numbered/1            200 '"title":"length"'
check "a ? inside a string" GET /question_mark          200 '"id"'

echo "== long text =="
check "4000-byte text" GET /length 200 '"recibido":4000'

echo "== types =="
check "integer al limite"   GET /types 200 '"t_big":-9223372036854775808'
check "booleano"           GET /types 200 '"t_bool":true'
check "utf8 text"        GET /types 200 'emoji'
check "fecha"              GET /types 200 '"t_date":"2026-08-31"'
check "json y jsonb"       GET /types 200 '"t_jsonb"'
check "uuid"               GET /types 200 '0000-0000-0000-000000000001'
check "array"              GET /types 200 '"t_array":"{1,2,3}"'
check "null"               GET /types 200 '"t_null":null'
# bytea: libpq already hands it over in postgres hex, so the driver does not
# need to decode it separately for it to be valid JSON -- what is checked
# is that this stays true and is not a silent coincidence.
check "bytea en hexadecimal" GET /types 200 '"t_bytea":"\\x68656c6c6f"'
json_valid "the whole types row is valid JSON" /types

# NaN and Infinity are valid in postgres and JSON cannot write them.
echo "== special floating point values =="
json_valid "the response with NaN and Infinity is valid JSON" /special

echo "== unicode e injection =="
check "unicode in the bind"      GET /unicode 200 'unicode'
check "quote in the parameter" GET "/injection?q=x'%20OR%20'1'='1" 200 '"encontrados":0'

echo "== escritura =="
check "insert y rows afectadas" POST /add/probing  201 '"rows":1'
check "returning id"             POST /add_id/other   201 '"id"'
check "delete"                   POST /delete_row/99999    200 '"rows":0'
# postgres has no reliable last_id: the driver says so instead of making one up.
check "last_id says it is unavailable" POST /last_id 200 'is not available'

echo "== transactions =="
check "commit"               POST /transfer       200 '"ok":true'
check "balances after commit"   GET  /balances           200 '"balance":130'
check "begin without error"      POST /undo_verbose  200 '"begin":true'
check "balances after rollback" GET  /balances           200 '"balance":70'

echo "== errors =="
check "missing table"    GET /bad_table          200 "no_existe"
check "too few placeholders"    GET /too_few_placeholders 200 "were passed"
check "mixedr ? y \$1"        GET /mixed              200 "mixed"

# A 200 is not enough: one request answering with ANOTHER's row is exactly
# the shape of the shared-bind failure already caught in mysql -- there it only
# shows up by comparing the content, not just the status code.
echo "== concurrency (pool 8) =="
fallos_conc=0
pids=""
for i in $(seq 1 40); do
    curl -s --max-time 10 -o "$TMP/c$i" -w '%{http_code}' \
      "http://127.0.0.1:$PORT/one/$(( (i % 3) + 1 ))" > "$TMP/s$i" &
    pids="$pids $!"
done
for p in $pids; do wait "$p" 2>/dev/null; done
for i in $(seq 1 40); do
    [ "$(cat "$TMP/s$i" 2>/dev/null)" = "200" ] || fallos_conc=$((fallos_conc + 1))
    case $(( (i % 3) + 1 )) in
        1) grep -q 'length'   "$TMP/c$i" || fallos_conc=$((fallos_conc + 1)) ;;
        2) grep -q 'short'   "$TMP/c$i" || fallos_conc=$((fallos_conc + 1)) ;;
        3) grep -q 'unicode' "$TMP/c$i" || fallos_conc=$((fallos_conc + 1)) ;;
    esac
done
if [ "$fallos_conc" -eq 0 ]; then ok "40 concurrent, each with its own result"
else fail "40 concurrent, each with its own result" "40 correctas" "$fallos_conc mal"; fi

kill -0 "$SRV" 2>/dev/null && ok "the server is still alive" \
                           || fail "the server is still alive" "vivo" "muerto"

echo
if [ "$failed" -eq 0 ]; then green "$passed tests, all passing"; exit 0; fi
red "$passed passed, $failed failed"
echo "--- server log ---"; tail -30 "$TMP/srv.log"
exit 1
