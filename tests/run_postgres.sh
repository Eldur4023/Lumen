#!/usr/bin/env bash
#
# Suite del modulo postgres de Lumen 2.0.
#
# Va aparte de run_tests.sh porque necesita un servidor: sin el, esta suite se
# SALTA sola en vez de fallar, para que `ctest` siga siendo verde en una maquina
# donde no hay postgres.  El codigo de salida 77 es el que CMake entiende como
# "omitida".
#
# Para levantar el entorno una vez.  Cada sentencia en su propia invocacion:
# `psql -c` con varias las envuelve en una transaccion, y CREATE DATABASE no
# puede correr dentro de una.
#
#   sudo apt install -y postgresql
#   sudo service postgresql start
#   sudo -u postgres psql -c "CREATE USER lumen WITH PASSWORD 'lumen';"
#   sudo -u postgres psql -c "CREATE DATABASE lumen_pruebas OWNER lumen;"
#
#   tests/run_postgres.sh [ruta-al-binario]
#
# Las credenciales estan escritas a proposito: es una base de pruebas que se
# recrea entera en cada pasada, igual que el fichero .db de la suite de sqlite.

set -u

LUMEN="${1:-$HOME/lumen-build/lumen}"
AQUI="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
PUERTO=${LUMEN_TEST_PG_PORT:-8810}
SRV=""

PGURL="postgresql://lumen:lumen@127.0.0.1/lumen_pruebas"

pasadas=0
fallidas=0

rojo()  { printf '\033[31m%s\033[0m\n' "$*"; }
verde() { printf '\033[32m%s\033[0m\n' "$*"; }
gris()  { printf '\033[90m%s\033[0m\n' "$*"; }

ok()   { pasadas=$((pasadas + 1)); printf '  ok    %s\n' "$1"; }
fallo() {
    fallidas=$((fallidas + 1))
    rojo "  FALLA $1"
    printf '        esperado: %s\n        obtenido: %s\n' "$2" "$3"
}

parar() {
    [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null
    # Solo ese pid: un `wait` sin argumentos esperaria tambien al servidor y
    # colgaria la suite entera.
    wait "$SRV" 2>/dev/null
    SRV=""
}
trap 'parar; rm -rf "$TMP"' EXIT

# ─── Se puede correr? ────────────────────────────────────────────────────────

if ! command -v psql > /dev/null 2>&1; then
    gris "postgres: no hay cliente instalado — suite omitida"
    exit 77
fi
if ! psql "$PGURL" -c "select 1" > /dev/null 2>&1; then
    gris "postgres: no se puede conectar a lumen_pruebas — suite omitida"
    gris "          (las instrucciones para montarlo estan en la cabecera de este fichero)"
    exit 77
fi
if ! "$LUMEN" --check "$AQUI/casos/postgres.lum" > "$TMP/check" 2>&1; then
    if grep -q "postgres" "$TMP/check" && grep -q "import" "$TMP/check"; then
        gris "postgres: el binario se compilo sin el modulo — suite omitida"
        exit 77
    fi
    rojo "el fichero de pruebas no compila:"; cat "$TMP/check"; exit 1
fi

# ─── Utilidades ──────────────────────────────────────────────────────────────

comprueba() {
    local nombre="$1" metodo="$2" ruta="$3" cod="$4" trozo="${5:-}"
    local got
    got=$(curl -sS --max-time 10 -X "$metodo" -o "$TMP/body" -w '%{http_code}' \
          "http://127.0.0.1:$PUERTO$ruta" 2>/dev/null)
    local body; body=$(head -c 400 "$TMP/body" 2>/dev/null)
    if [ "$got" != "$cod" ]; then
        fallo "$nombre" "codigo $cod" "codigo $got — $body"; return
    fi
    if [ -n "$trozo" ] && ! grep -qF "$trozo" "$TMP/body"; then
        fallo "$nombre" "que contenga '$trozo'" "$body"; return
    fi
    ok "$nombre"
}

# Comprueba que el cuerpo de la respuesta es JSON valido de verdad.  Un NaN o un
# Infinity de postgres saldrian como `nan` o `inf`, que ningun cliente sabe leer.
json_valido() {
    local nombre="$1" ruta="$2"
    curl -sS --max-time 10 -o "$TMP/body" "http://127.0.0.1:$PUERTO$ruta" 2>/dev/null
    if python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$TMP/body" 2>/dev/null; then
        ok "$nombre"
    else
        fallo "$nombre" "JSON valido" "$(head -c 200 "$TMP/body")"
    fi
}

# ─── Arranque ────────────────────────────────────────────────────────────────

psql "$PGURL" -q -v ON_ERROR_STOP=1 -f "$AQUI/casos/postgres-esquema.sql" > /dev/null 2>&1 || {
    rojo "no se puede cargar el esquema:"
    psql "$PGURL" -v ON_ERROR_STOP=1 -f "$AQUI/casos/postgres-esquema.sql" 2>&1 | tail -5
    exit 1; }

echo "== arranque =="
"$LUMEN" --no-watch --port "$PUERTO" "$AQUI/casos/postgres.lum" > "$TMP/srv.log" 2>&1 &
SRV=$!
for _ in $(seq 1 60); do
    curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PUERTO/__ping__" 2>/dev/null && break
    kill -0 "$SRV" 2>/dev/null || { rojo "el servidor murio al arrancar:"; cat "$TMP/srv.log"; exit 1; }
    sleep 0.3
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PUERTO/__ping__" || {
    rojo "el servidor no respondio"; cat "$TMP/srv.log"; exit 1; }
ok "arranca y conecta"

echo "== lectura =="
comprueba "select sin parametros" GET /todos      200 '"titulo":"largo"'
comprueba "select con parametro"  GET /uno/1      200 '"autor":"Ana"'
comprueba "fila que no existe"    GET /uno/99999  404

# El marcador de Lumen Script es `?` y el driver lo traduce a $1: aqui se comprueba
# contra un servidor de verdad, no solo contra la funcion suelta.
echo "== marcadores =="
comprueba "dos marcadores en orden" GET '/dos?autor=Ana&vistas=0' 200 '"titulo":"largo"'
comprueba "estilo \$1 sigue valiendo" GET /numerado/1            200 '"titulo":"largo"'
comprueba "un ? dentro de una cadena" GET /interrogante          200 '"id"'

echo "== texto largo =="
comprueba "text de 4000 bytes entero" GET /largo 200 '"recibido":4000'

echo "== tipos =="
comprueba "entero al limite"   GET /tipos 200 '"t_big":-9223372036854775808'
comprueba "booleano"           GET /tipos 200 '"t_bool":true'
comprueba "cadena utf8"        GET /tipos 200 'emoji'
comprueba "fecha"              GET /tipos 200 '"t_date":"2026-08-31"'
comprueba "json y jsonb"       GET /tipos 200 '"t_jsonb"'
comprueba "uuid"               GET /tipos 200 '0000-0000-0000-000000000001'
comprueba "array"              GET /tipos 200 '"t_array":"{1,2,3}"'
comprueba "nulo"               GET /tipos 200 '"t_nulo":null'
# bytea: libpq lo entrega ya en el hexadecimal de postgres, asi que el driver
# no necesita decodificarlo aparte para que sea JSON valido -- lo que se
# comprueba es que eso siga siendo cierto y no una casualidad silenciosa.
comprueba "bytea en hexadecimal" GET /tipos 200 '"t_bytea":"\\x68656c6c6f"'
json_valido "la fila de tipos entera es JSON valido" /tipos

# NaN e Infinity son validos en postgres y JSON no los sabe escribir.
echo "== valores especiales de coma flotante =="
json_valido "la respuesta con NaN e Infinity es JSON valido" /especiales

echo "== unicode e inyeccion =="
comprueba "unicode en el bind"      GET /unicode 200 'unicode'
comprueba "comilla en el parametro" GET "/inyeccion?q=x'%20OR%20'1'='1" 200 '"encontrados":0'

echo "== escritura =="
comprueba "insert y filas afectadas" POST /alta/probando  201 '"filas":1'
comprueba "returning id"             POST /alta_id/otro   201 '"id"'
comprueba "delete"                   POST /borra/99999    200 '"filas":0'
# postgres no tiene un last_id fiable: el driver lo dice en vez de inventarselo.
comprueba "last_id avisa de que no esta" POST /last_id 200 'no esta disponible'

echo "== transacciones =="
comprueba "commit"               POST /transfiere       200 '"ok":true'
comprueba "saldos tras commit"   GET  /saldos           200 '"saldo":130'
comprueba "begin sin error"      POST /deshace_verboso  200 '"begin":true'
comprueba "saldos tras rollback" GET  /saldos           200 '"saldo":70'

echo "== errores =="
comprueba "tabla que no existe"    GET /tabla_mala          200 "no_existe"
comprueba "marcadores de menos"    GET /marcadores_de_menos 200 "marcador"
comprueba "mezclar ? y \$1"        GET /mezcla              200 "mezcla"

# Un 200 no basta: una peticion contestando con la fila de OTRA es exactamente
# la forma que tenia el fallo de bind compartido que ya se caza en mysql -- ahi
# solo se ve comparando el contenido, no solo el codigo.
echo "== concurrencia (pool 8) =="
fallos_conc=0
pids=""
for i in $(seq 1 40); do
    curl -s --max-time 10 -o "$TMP/c$i" -w '%{http_code}' \
      "http://127.0.0.1:$PUERTO/uno/$(( (i % 3) + 1 ))" > "$TMP/s$i" &
    pids="$pids $!"
done
for p in $pids; do wait "$p" 2>/dev/null; done
for i in $(seq 1 40); do
    [ "$(cat "$TMP/s$i" 2>/dev/null)" = "200" ] || fallos_conc=$((fallos_conc + 1))
    case $(( (i % 3) + 1 )) in
        1) grep -q 'largo'   "$TMP/c$i" || fallos_conc=$((fallos_conc + 1)) ;;
        2) grep -q 'corto'   "$TMP/c$i" || fallos_conc=$((fallos_conc + 1)) ;;
        3) grep -q 'unicode' "$TMP/c$i" || fallos_conc=$((fallos_conc + 1)) ;;
    esac
done
if [ "$fallos_conc" -eq 0 ]; then ok "40 simultaneas, cada una con su resultado"
else fallo "40 simultaneas, cada una con su resultado" "40 correctas" "$fallos_conc mal"; fi

kill -0 "$SRV" 2>/dev/null && ok "el servidor sigue vivo" \
                           || fallo "el servidor sigue vivo" "vivo" "muerto"

echo
if [ "$fallidas" -eq 0 ]; then verde "$pasadas pruebas, todas pasan"; exit 0; fi
rojo "$pasadas pasan, $fallidas fallan"
echo "--- log del servidor ---"; tail -30 "$TMP/srv.log"
exit 1
