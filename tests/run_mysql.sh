#!/usr/bin/env bash
#
# Suite for Lumen's mysql module.
#
# It is separate from run_tests.sh because it needs a server: without one, this suite
# it SKIPS itself instead of failing, so `ctest` stays green on a machine
# with no mysqld.  Exit code 77 is the one CMake understands as
# "omitida".
#
# To set the environment up once:
#
#   sudo apt install -y mysql-server
#   sudo service mysql start
#   sudo mysql -e "CREATE DATABASE lumen_tests CHARACTER SET utf8mb4;
#                  CREATE USER 'lumen'@'127.0.0.1' IDENTIFIED BY 'lumen';
#                  GRANT ALL ON lumen_tests.* TO 'lumen'@'127.0.0.1';
#                  FLUSH PRIVILEGES;"
#
#   tests/run_mysql.sh [path-to-binary]
#
# The credentials are written in on purpose: it is a test database
# recreated from scratch on every pass, just like the .db file of the
# sqlite.

set -u

LUMEN="${1:-$HOME/lumen-build/lumen}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
PORT=${LUMEN_TEST_MYSQL_PORT:-8800}
SRV=""

DB_HOST=127.0.0.1
DB_USER=lumen
DB_PASS=lumen
DB_NAME=lumen_tests

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

if ! command -v mysql > /dev/null 2>&1; then
    grey "mysql: no client installed — suite skipped"
    exit 77
fi
if ! mysql -h "$DB_HOST" -u "$DB_USER" "-p$DB_PASS" "$DB_NAME" \
        -e "select 1" > /dev/null 2>&1; then
    grey "mysql: cannot connect to $DB_NAME on $DB_HOST — suite skipped"
    grey "       (the instructions for setting it up are in this file's header)"
    exit 77
fi
if ! "$LUMEN" --check "$HERE/cases/mysql.lum" > "$TMP/check" 2>&1; then
    if grep -q "modulo 'mysql'" "$TMP/check" || grep -q "import mysql" "$TMP/check"; then
        grey "mysql: the binary was built without the module — suite skipped"
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

# Really valid JSON, UTF-8 included.  The BLOB of /types is exactly the
# data type that already broke this once -- checking only the expected value
# is not enough, because it would not catch a regression in ANOTHER column of the same row.
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

# ─── Arranque ────────────────────────────────────────────────────────────────

mysql -h "$DB_HOST" -u "$DB_USER" "-p$DB_PASS" "$DB_NAME" \
      < "$HERE/cases/mysql-schema.sql" 2>/dev/null || {
    red "cannot load the schema"; exit 1; }

echo "== arranque =="
"$LUMEN" --no-watch --port "$PORT" "$HERE/cases/mysql.lum" > "$TMP/srv.log" 2>&1 &
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

# A 4000-byte TEXT.  The output buffers were sized with field.max_length,
# which is zero unless it is asked for and the result is fetched: anything
# past 1023 bytes was silently truncated.
echo "== long text =="
check "TEXT de 4000 bytes integer" GET /length 200 '"recibido":4000'

echo "== types =="
check "signed integer"   GET /types 200 '"t_big":-9223372036854775808'
check "decimal"            GET /types 200 '"t_decimal":12345678.9012'
check "utf8 textmb4"     GET /types 200 'emoji'
check "fecha"              GET /types 200 '"t_date":"2026-08-31"'
check "json"               GET /types 200 '"t_json"'
check "null"               GET /types 200 '"t_null":null'
# A BLOB is arbitrary bytes, not text: it comes out in base64 so the response
# siga siendo UTF-8 valido.  'bytes' -> 'Ynl0ZXM='.
check "the blob comes out in base64" GET /types 200 '"t_blob":"Ynl0ZXM="'
# Above 2^63 a BIGINT UNSIGNED does not fit in the Lumen Script integer and falls
# back to a decimal, as in the JSON parser: the last digit is lost.  It used to
# get pinned at INT64_MAX, which is half the value.
check "unsigned bigint"   GET /types 200 '"t_ubig":1844674407370955'
json_valid "the whole types row is valid JSON" /types

echo "== null byte inside a text =="
check "the driver returns the 5 bytes" GET /null_in_text 200 '"bytes":5'
json_valid "and the JSON stays valid" /null_in_text

echo "== params =="
check "echo de params"  GET '/echo?n=42&s=abc' 200 '"integer"'
check "unicode in the bind" GET /unicode          200 'unicode'

echo "== injection =="
check "quote in the parameter" GET "/injection?q=x'%20OR%20'1'='1" 200 '"encontrados":0'

echo "== escritura =="
check "insert and last_id" POST /add/probing 201 '"id"'
check "delete"           POST /delete_row/99999   200 '"ok":true'

# The rollback is checked by looking at what EACH step returns, not just the
# final 200: begin() failed and its error was discarded, so the transaction
# never opened and the rollback answered yes without undoing anything.
echo "== transactions =="
check "commit"               POST /transfer       200 '"ok":true'
check "balances after commit"   GET  /balances           200 '"balance":130'
check "begin without error"      POST /undo_verbose  200 '"begin":true'
check "balances after rollback" GET  /balances           200 '"balance":70'

# An engine error arrives as a value with 200, as in sqlite: that is the
# design decision.  What is checked is that the message is useful and that
# the server stays up.
echo "== engine errors =="
check "missing table"  GET /bad_table          200 "no_existe"
check "too few parameters"  GET /too_few_params 200 "were passed"

# The bind buffers used to live in the driver, which is a single one shared by
# the N pool workers: the real race is already caught with ThreadSanitizer, but
# a 200 is not enough here -- one request answering with ANOTHER's row is
# exactly the shape that failure had before it was fixed, and it only shows
# comparing the content, not just the status code.
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
