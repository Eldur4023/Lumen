#!/usr/bin/env bash
#
# Bateria de tipos del modulo sqlite de Lumen 2.0.
#
# Va aparte de run_tests.sh —que ya ejerce sqlite en el uso normal— porque esto
# es otra cosa: los limites de los tipos, lo que sqlite guarda de verdad frente a
# lo que dice la declaracion de la columna, y la cache de sentencias.
#
# No necesita servidor ni cliente: el esquema lo crea el propio .lum, asi que
# la suite solo depende del binario y corre siempre.
#
#   tests/run_sqlite.sh [ruta-al-binario]

set -u

LUMEN="${1:-$HOME/lumen-build/lumen}"
# A ruta absoluta antes de nada: mas abajo se cambia de directorio para que el
# fichero .db caiga en la raiz del repo, y una ruta relativa dejaria de valer.
case "$LUMEN" in /*) ;; *) LUMEN="$(cd "$(dirname "$LUMEN")" && pwd)/$(basename "$LUMEN")" ;; esac
AQUI="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
PUERTO=${LUMEN_TEST_SQLITE_PORT:-8820}
SRV=""

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
    # Solo ese pid: un `wait` sin argumentos esperaria tambien al servidor.
    wait "$SRV" 2>/dev/null
    SRV=""
}
trap 'parar; rm -rf "$TMP" "$AQUI/../pruebas-sqlite-tipos.db"* 2>/dev/null' EXIT

if ! "$LUMEN" --check "$AQUI/casos/sqlite.lum" > "$TMP/check" 2>&1; then
    if grep -q "sqlite" "$TMP/check" && grep -q "import" "$TMP/check"; then
        gris "sqlite: el binario se compilo sin el modulo — suite omitida"
        exit 77
    fi
    rojo "el fichero de pruebas no compila:"; cat "$TMP/check"; exit 1
fi

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

# JSON valido de verdad: UTF-8 correcto incluido.  Un blob con bytes sueltos o
# un infinito saldrian de forma que ningun cliente sabe leer.
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

rm -f "$AQUI/../pruebas-sqlite-tipos.db"*

echo "== arranque =="
cd "$AQUI/.."
"$LUMEN" --no-watch --port "$PUERTO" "$AQUI/casos/sqlite.lum" > "$TMP/srv.log" 2>&1 &
SRV=$!
for _ in $(seq 1 60); do
    curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PUERTO/__ping__" 2>/dev/null && break
    kill -0 "$SRV" 2>/dev/null || { rojo "el servidor murio al arrancar:"; cat "$TMP/srv.log"; exit 1; }
    sleep 0.3
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PUERTO/__ping__" || {
    rojo "el servidor no respondio"; cat "$TMP/srv.log"; exit 1; }
ok "arranca y conecta"
comprueba "crea el esquema" GET /crear 200 '"ok":true'

echo "== lectura =="
comprueba "select sin parametros" GET /todos      200 '"titulo":"largo"'
comprueba "select con parametro"  GET /uno/1      200 '"autor":"Ana"'
comprueba "fila que no existe"    GET /uno/99999  404

echo "== texto largo =="
comprueba "text de 4000 bytes entero" GET /largo 200 '"recibido":4000'

echo "== tipos =="
comprueba "entero maximo"  GET /tipos 200 '"t_int":9223372036854775807'
comprueba "entero minimo"  GET /tipos 200 '"t_neg":-9223372036854775808'
comprueba "decimal"        GET /tipos 200 '"t_real":3.141592653589793'
comprueba "cadena utf8"    GET /tipos 200 'emoji'
comprueba "nulo"           GET /tipos 200 '"t_nulo":null'
# El blob lleva x'00FF80FE': bytes que no forman UTF-8 valido.  Salen en base64.
json_valido "un blob con bytes sueltos no rompe el JSON" /tipos
comprueba "el blob sale en base64" GET /tipos 200 '"t_blob":"AP+A/g=="'

echo "== byte nulo dentro de un texto =="
comprueba "el driver devuelve los 5 bytes" GET /nulo_en_texto 200 '"bytes":5'
# length() del motor cuenta hasta el primer NUL: es su definicion, no un fallo.
comprueba "length() de sqlite para en el nulo" GET /nulo_en_texto 200 '"hasta_el_nulo":1'
json_valido "y el JSON sigue siendo valido" /nulo_en_texto

echo "== afinidad de tipos =="
# sqlite no impone el tipo declarado: una columna integer puede guardar texto, y
# lo que sale tiene que ser lo que HAY, no lo que dice la declaracion.
comprueba "texto en columna integer" GET /afinidad 200 '"tipo":"text"'
comprueba "entero en la misma"       GET /afinidad 200 '"tipo":"integer"'

echo "== infinito =="
json_valido "un infinito no rompe el JSON" /infinito

echo "== cache de sentencias =="
# La misma consulta se prepara una vez y se reutiliza: si al reutilizarla no se
# limpiaran los enlaces, la segunda llamada devolveria el resultado de la
# primera.
comprueba "primera vez"       GET /repetida/1 200 '"titulo":"largo"'
comprueba "segunda, otro id"  GET /repetida/2 200 '"titulo":"corto"'
comprueba "tercera, el primero otra vez" GET /repetida/1 200 '"titulo":"largo"'
comprueba "cuarta, id que no existe"     GET /repetida/9999 200 '"titulo":null'
comprueba "misma consulta con null"      GET /opcional 200 '"n":0'
comprueba "misma consulta con valor"     GET '/opcional?autor=Ana' 200 '"n":1'

echo "== unicode e inyeccion =="
comprueba "unicode en el bind"      GET /unicode 200 'unicode'
comprueba "comilla en el parametro" GET "/inyeccion?q=x'%20OR%20'1'='1" 200 '"encontrados":0'

echo "== escritura =="
comprueba "insert y last_id" POST /alta/probando 201 '"id"'
comprueba "delete"           POST /borra/99999   200 '"filas":0'

echo "== transacciones =="
comprueba "commit"               POST /transfiere       200 '"ok":true'
comprueba "saldos tras commit"   GET  /saldos           200 '"saldo":130'
comprueba "begin sin error"      POST /deshace_verboso  200 '"begin":true'
comprueba "saldos tras rollback" GET  /saldos           200 '"saldo":70'

echo "== errores =="
comprueba "tabla que no existe" GET /tabla_mala          200 "no_existe"
comprueba "parametros de menos" GET /parametros_de_menos 200 "parametro"

echo "== concurrencia (pool 8) =="
# La cache de sentencias es por worker.  Aqui se comprueba que N workers usando
# la MISMA consulta con parametros distintos no se pisan.
fallos_conc=0
pids=""
for i in $(seq 1 40); do
    curl -s --max-time 10 -o "$TMP/c$i" -w '%{http_code}' \
      "http://127.0.0.1:$PUERTO/repetida/$(( (i % 3) + 1 ))" > "$TMP/s$i" &
    pids="$pids $!"
done
for p in $pids; do wait "$p" 2>/dev/null; done
for i in $(seq 1 40); do
    [ "$(cat "$TMP/s$i" 2>/dev/null)" = "200" ] || fallos_conc=$((fallos_conc + 1))
    esperado=$(( (i % 3) + 1 ))
    case $esperado in
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
