#!/usr/bin/env bash
#
# Suite del modulo mysql de Lumen 2.0.
#
# Va aparte de run_tests.sh porque necesita un servidor: sin el, esta suite se
# SALTA sola en vez de fallar, para que `ctest` siga siendo verde en una maquina
# donde no hay mysqld.  El codigo de salida 77 es el que CMake entiende como
# "omitida".
#
# Para levantar el entorno una vez:
#
#   sudo apt install -y mysql-server
#   sudo service mysql start
#   sudo mysql -e "CREATE DATABASE lumen_pruebas CHARACTER SET utf8mb4;
#                  CREATE USER 'lumen'@'127.0.0.1' IDENTIFIED BY 'lumen';
#                  GRANT ALL ON lumen_pruebas.* TO 'lumen'@'127.0.0.1';
#                  FLUSH PRIVILEGES;"
#
#   tests/run_mysql.sh [ruta-al-binario]
#
# Las credenciales estan escritas a proposito: es una base de datos de pruebas
# que se recrea entera en cada pasada, igual que el fichero .db de la suite de
# sqlite.

set -u

LUMEN="${1:-$HOME/lumen-build/lumen}"
AQUI="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
PUERTO=${LUMEN_TEST_MYSQL_PORT:-8800}
SRV=""

DB_HOST=127.0.0.1
DB_USER=lumen
DB_PASS=lumen
DB_NOMBRE=lumen_pruebas

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

if ! command -v mysql > /dev/null 2>&1; then
    gris "mysql: no hay cliente instalado — suite omitida"
    exit 77
fi
if ! mysql -h "$DB_HOST" -u "$DB_USER" "-p$DB_PASS" "$DB_NOMBRE" \
        -e "select 1" > /dev/null 2>&1; then
    gris "mysql: no se puede conectar a $DB_NOMBRE en $DB_HOST — suite omitida"
    gris "       (las instrucciones para montarlo estan en la cabecera de este fichero)"
    exit 77
fi
if ! "$LUMEN" --check "$AQUI/casos/mysql.lum" > "$TMP/check" 2>&1; then
    if grep -q "modulo 'mysql'" "$TMP/check" || grep -q "import mysql" "$TMP/check"; then
        gris "mysql: el binario se compilo sin el modulo — suite omitida"
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

# JSON valido de verdad, UTF-8 incluido.  El BLOB de /tipos es exactamente el
# tipo de dato que ya rompio esto una vez -- comprobar solo el valor esperado
# no basta, porque no cazaria una regresion en OTRA columna de la misma fila.
json_valido() {
    local nombre="$1" ruta="$2"
    curl -sS --max-time 10 -o "$TMP/body" "http://127.0.0.1:$PUERTO$ruta" 2>/dev/null
    if python3 -c "import json,sys; json.load(open(sys.argv[1], encoding='utf-8'))" \
            "$TMP/body" 2>/dev/null; then
        ok "$nombre"
    else
        fallo "$nombre" "JSON valido en UTF-8" "$(head -c 200 "$TMP/body" | cat -v)"
    fi
}

# ─── Arranque ────────────────────────────────────────────────────────────────

mysql -h "$DB_HOST" -u "$DB_USER" "-p$DB_PASS" "$DB_NOMBRE" \
      < "$AQUI/casos/mysql-esquema.sql" 2>/dev/null || {
    rojo "no se puede cargar el esquema"; exit 1; }

echo "== arranque =="
"$LUMEN" --no-watch --port "$PUERTO" "$AQUI/casos/mysql.lum" > "$TMP/srv.log" 2>&1 &
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

# Un TEXT de 4000 bytes.  Los buffers de salida se dimensionaban con
# field.max_length, que vale cero mientras no se pida y no se traiga el
# resultado: todo lo que pasara de 1023 bytes se truncaba en silencio.
echo "== texto largo =="
comprueba "TEXT de 4000 bytes entero" GET /largo 200 '"recibido":4000'

echo "== tipos =="
comprueba "entero con signo"   GET /tipos 200 '"t_big":-9223372036854775808'
comprueba "decimal"            GET /tipos 200 '"t_decimal":12345678.9012'
comprueba "cadena utf8mb4"     GET /tipos 200 'emoji'
comprueba "fecha"              GET /tipos 200 '"t_date":"2026-08-31"'
comprueba "json"               GET /tipos 200 '"t_json"'
comprueba "nulo"               GET /tipos 200 '"t_nulo":null'
# Un BLOB son bytes cualesquiera, no texto: sale en base64 para que la respuesta
# siga siendo UTF-8 valido.  'bytes' -> 'Ynl0ZXM='.
comprueba "el blob sale en base64" GET /tipos 200 '"t_blob":"Ynl0ZXM="'
# Por encima de 2^63 un BIGINT UNSIGNED no cabe en el entero de Lumen Script y cae a
# decimal, igual que en el parser de JSON: se pierde el ultimo digito.  Antes se
# quedaba clavado en INT64_MAX, que es la mitad del valor.
comprueba "bigint sin signo"   GET /tipos 200 '"t_ubig":1844674407370955'
json_valido "la fila de tipos entera es JSON valido" /tipos

echo "== byte nulo dentro de un texto =="
comprueba "el driver devuelve los 5 bytes" GET /nulo_en_texto 200 '"bytes":5'
json_valido "y el JSON sigue siendo valido" /nulo_en_texto

echo "== parametros =="
comprueba "eco de parametros"  GET '/eco?n=42&s=abc' 200 '"entero"'
comprueba "unicode en el bind" GET /unicode          200 'unicode'

echo "== inyeccion =="
comprueba "comilla en el parametro" GET "/inyeccion?q=x'%20OR%20'1'='1" 200 '"encontrados":0'

echo "== escritura =="
comprueba "insert y last_id" POST /alta/probando 201 '"id"'
comprueba "delete"           POST /borra/99999   200 '"ok":true'

# El rollback se comprueba mirando lo que devuelve CADA paso, no solo el 200
# final: begin() fallaba y su error se descartaba, asi que la transaccion no se
# abria y el rollback contestaba que si sin deshacer nada.
echo "== transacciones =="
comprueba "commit"               POST /transfiere       200 '"ok":true'
comprueba "saldos tras commit"   GET  /saldos           200 '"saldo":130'
comprueba "begin sin error"      POST /deshace_verboso  200 '"begin":true'
comprueba "saldos tras rollback" GET  /saldos           200 '"saldo":70'

# Un error del motor llega como valor con 200, igual que en sqlite: es la
# decision de diseno.  Lo que se comprueba es que el mensaje sirva de algo y que
# el servidor siga en pie.
echo "== errores del motor =="
comprueba "tabla que no existe"  GET /tabla_mala          200 "no_existe"
comprueba "parametros de menos"  GET /parametros_de_menos 200 "parametro"

# Los buffers de bind vivian en el driver, que es unico y lo comparten los N
# workers del pool: la carrera real ya se caza con ThreadSanitizer, pero un
# 200 no basta aqui -- una peticion contestando con la fila de OTRA es
# exactamente la forma que tenia ese fallo antes de arreglarlo, y solo se ve
# comparando el contenido, no solo el codigo.
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
