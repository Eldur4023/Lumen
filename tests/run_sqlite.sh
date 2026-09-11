#!/usr/bin/env bash
#
# Type battery for Lumen's sqlite module.
#
# It is separate from run_tests.sh —which already drives sqlite in normal use—
# because this is another thing: the type limits, what sqlite really stores
# versus what the column declaration says, and the statement cache.
#
# It needs no server and no client: the schema is created by the .lum itself, so
# the suite depends only on the binary and always runs.
#
#   tests/run_sqlite.sh [path-to-binary]

set -u

LUMEN="${1:-$HOME/lumen-build/lumen}"
# An absolute path first of all: further down the directory is changed so the
# .db file lands in the repo root, and a relative path would stop working.
case "$LUMEN" in /*) ;; *) LUMEN="$(cd "$(dirname "$LUMEN")" && pwd)/$(basename "$LUMEN")" ;; esac
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
PORT=${LUMEN_TEST_SQLITE_PORT:-8820}
SRV=""

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
    # Only that pid: a bare `wait` would also wait for the server.
    wait "$SRV" 2>/dev/null
    SRV=""
}
trap 'stop_server; rm -rf "$TMP" "$HERE/../tests-sqlite-types.db"* 2>/dev/null' EXIT

if ! "$LUMEN" --check "$HERE/cases/sqlite.lum" > "$TMP/check" 2>&1; then
    if grep -q "sqlite" "$TMP/check" && grep -q "import" "$TMP/check"; then
        grey "sqlite: the binary was built without the module — suite skipped"
        exit 77
    fi
    red "the test file does not compile:"; cat "$TMP/check"; exit 1
fi

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

# Really valid JSON: correct UTF-8 included.  A blob with stray bytes or an
# infinity would come out in a shape no client can read.
json_valid() {
    local name="$1" path="$2"
    curl -sS --max-time 10 -o "$TMP/body" "http://127.0.0.1:$PORT$path" 2>/dev/null
    if python3 -c "import json,sys; json.load(open(sys.argv[1], encoding='utf-8'))" \
            "$TMP/body" 2>/dev/null; then
        ok "$name"
    else
        fail "$name" "valid UTF-8 JSON" "$(head -c 200 "$TMP/body" | cat -v)"
    fi
}

rm -f "$HERE/../tests-sqlite-types.db"*

echo "== arranque =="
cd "$HERE/.."
"$LUMEN" --no-watch --port "$PORT" "$HERE/cases/sqlite.lum" > "$TMP/srv.log" 2>&1 &
SRV=$!
for _ in $(seq 1 60); do
    curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/__ping__" 2>/dev/null && break
    kill -0 "$SRV" 2>/dev/null || { red "the server died on startup:"; cat "$TMP/srv.log"; exit 1; }
    sleep 0.3
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/__ping__" || {
    red "the server did not answer"; cat "$TMP/srv.log"; exit 1; }
ok "starts and connects"
check "creates the schema" GET /create 200 '"ok":true'

echo "== lectura =="
check "select without parameters" GET /all      200 '"title":"long"'
check "select with a parameter"  GET /one/1      200 '"author":"Ana"'
check "missing row"    GET /one/99999  404

echo "== long text =="
check "4000-byte text" GET /length 200 '"recibido":4000'

echo "== types =="
check "integer maximum"  GET /types 200 '"t_int":9223372036854775807'
check "integer minimum"  GET /types 200 '"t_neg":-9223372036854775808'
check "decimal"        GET /types 200 '"t_real":3.141592653589793'
check "utf8 text"    GET /types 200 'emoji'
check "null"           GET /types 200 '"t_null":null'
# The blob carries x'00FF80FE': bytes that are not valid UTF-8.  They come out in base64.
json_valid "a blob with stray bytes does not break the JSON" /types
check "the blob comes out in base64" GET /types 200 '"t_blob":"AP+A/g=="'

echo "== null byte inside a text =="
check "the driver returns the 5 bytes" GET /null_in_text 200 '"bytes":5'
# The engine's length() counts up to the first NUL: that is its definition, not a fault.
check "sqlite's length() stops at the null" GET /null_in_text 200 '"up_to_null":1'
json_valid "and the JSON stays valid" /null_in_text

echo "== affinity de types =="
# sqlite does not enforce the declared type: an integer column can hold text, and
# what comes out has to be what IS THERE, not what the declaration says.
check "text in an integer column" GET /affinity 200 '"type":"text"'
check "integer in the same one"       GET /affinity 200 '"type":"integer"'

echo "== infinite =="
json_valid "an infinity does not break the JSON" /infinite

echo "== cache de sentencias =="
# The same query is prepared once and reused: if the bindings were not cleared
# on reuse, the second call would return the result of the first
# one.
check "first time"       GET /repeated/1 200 '"title":"long"'
check "second, another id"  GET /repeated/2 200 '"title":"short"'
check "third, the first one again" GET /repeated/1 200 '"title":"long"'
check "fourth, missing id"     GET /repeated/9999 200 '"title":null'
check "same query with null"      GET /optional 200 '"n":0'
check "same query with a value"     GET '/optional?author=Ana' 200 '"n":1'

echo "== unicode e injection =="
check "unicode in the bind"      GET /unicode 200 'unicode'
check "quote in the parameter" GET "/injection?q=x'%20OR%20'1'='1" 200 '"encontrados":0'

echo "== escritura =="
check "insert and last_id" POST /add/probing 201 '"id"'
check "delete"           POST /delete_row/99999   200 '"rows":0'

echo "== transactions =="
check "commit"               POST /transfer       200 '"ok":true'
check "balances after commit"   GET  /balances           200 '"balance":130'
check "begin without error"      POST /undo_verbose  200 '"begin":true'
check "balances after rollback" GET  /balances           200 '"balance":70'

echo "== errors =="
check "missing table" GET /bad_table          200 "no_existe"
check "too few parameters" GET /too_few_params 200 "were passed"

echo "== concurrency (pool 8) =="
# The statement cache is per worker.  Here it is checked that N workers using
# the SAME query with different parameters do not tread on each other.
conc_failures=0
pids=""
for i in $(seq 1 40); do
    curl -s --max-time 10 -o "$TMP/c$i" -w '%{http_code}' \
      "http://127.0.0.1:$PORT/repeated/$(( (i % 3) + 1 ))" > "$TMP/s$i" &
    pids="$pids $!"
done
for p in $pids; do wait "$p" 2>/dev/null; done
for i in $(seq 1 40); do
    [ "$(cat "$TMP/s$i" 2>/dev/null)" = "200" ] || conc_failures=$((conc_failures + 1))
    expected=$(( (i % 3) + 1 ))
    case $expected in
        1) grep -q 'long'   "$TMP/c$i" || conc_failures=$((conc_failures + 1)) ;;
        2) grep -q 'short'   "$TMP/c$i" || conc_failures=$((conc_failures + 1)) ;;
        3) grep -q 'unicode' "$TMP/c$i" || conc_failures=$((conc_failures + 1)) ;;
    esac
done
if [ "$conc_failures" -eq 0 ]; then ok "40 concurrent, each with its own result"
else fail "40 concurrent, each with its own result" "40 correct" "$conc_failures wrong"; fi

kill -0 "$SRV" 2>/dev/null && ok "the server is still alive" \
                           || fail "the server is still alive" "alive" "dead"

echo
if [ "$failed" -eq 0 ]; then green "$passed tests, all passing"; exit 0; fi
red "$passed passed, $failed failed"
echo "--- server log ---"; tail -30 "$TMP/srv.log"
exit 1
