#!/usr/bin/env bash
#
# Suite de regresion de Lumen 2.0.
#
# Levanta el binario contra ficheros .lum reales y comprueba las respuestas.
# Esta escrita en shell a proposito: prueba el binario tal y como se usa, por el
# socket, sin enlazar nada del proyecto.
#
#   tests/run_tests.sh [ruta-al-binario]
#
# Sale con 0 si todo pasa.

set -u

LUMEN="${1:-$HOME/lumen-build/lumen}"
AQUI="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
PUERTO=${LUMEN_TEST_PORT:-8790}
SRV=""

pasadas=0
fallidas=0

# ─── Utilidades ──────────────────────────────────────────────────────────────

rojo()  { printf '\033[31m%s\033[0m\n' "$*"; }
verde() { printf '\033[32m%s\033[0m\n' "$*"; }

ok()   { pasadas=$((pasadas + 1)); printf '  ok    %s\n' "$1"; }
fallo() {
    fallidas=$((fallidas + 1))
    rojo "  FALLA $1"
    printf '        esperado: %s\n        obtenido: %s\n' "$2" "$3"
}

# Arranca un servidor con el .lum dado y espera a que responda.
# Cada suite usa su propio puerto: con SO_REUSEPORT dos procesos comparten
# puerto y el kernel reparte conexiones entre ambos, lo que falsearia todo.
levantar() {
    parar
    PUERTO=$((PUERTO + 1))
    "$LUMEN" --no-watch --port "$PUERTO" "$1" > "$TMP/srv.log" 2>&1 &
    SRV=$!
    for _ in $(seq 1 60); do
        if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PUERTO/__ping__" 2>/dev/null; then
            return 0
        fi
        kill -0 "$SRV" 2>/dev/null || { rojo "el servidor murio al arrancar:"; cat "$TMP/srv.log"; return 1; }
        sleep 0.2
    done
    rojo "el servidor no respondio"; cat "$TMP/srv.log"; return 1
}

parar() {
    [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null
    # Esperar SOLO a ese pid: un `wait` sin argumentos esperaria tambien al
    # servidor y colgaria la suite.
    wait "$SRV" 2>/dev/null
    SRV=""
}

# comprueba <nombre> <metodo> <ruta> <codigo-esperado> [subcadena-esperada] [datos]
comprueba() {
    local nombre="$1" metodo="$2" ruta="$3" cod="$4" trozo="${5:-}" datos="${6:-}"
    local args=(-sS --max-time 10 -X "$metodo" -o "$TMP/body" -w '%{http_code}')
    [ -n "$datos" ] && args+=(-H 'Content-Type: application/json' -d "$datos")

    local got
    got=$(curl "${args[@]}" "http://127.0.0.1:$PUERTO$ruta" 2>/dev/null)
    local body
    body=$(cat "$TMP/body" 2>/dev/null)

    if [ "$got" != "$cod" ]; then
        fallo "$nombre" "codigo $cod" "codigo $got — $body"
        return
    fi
    if [ -n "$trozo" ] && ! grep -qF "$trozo" "$TMP/body"; then
        fallo "$nombre" "que contenga '$trozo'" "$body"
        return
    fi
    ok "$nombre"
}

# comprueba_mp <nombre> <ruta> <codigo-esperado> <subcadena-esperada> -- <flags -F de curl...>
# Aparte de comprueba(): un cuerpo multipart no es una cadena que se pueda pasar
# con -d, son partes con nombre, y curl las arma con -F por campo.
comprueba_mp() {
    local nombre="$1" ruta="$2" cod="$3" trozo="${4:-}"; shift 4
    local got
    got=$(curl -sS --max-time 10 -X POST -o "$TMP/body" -w '%{http_code}' "$@" \
          "http://127.0.0.1:$PUERTO$ruta" 2>/dev/null)
    local body
    body=$(cat "$TMP/body" 2>/dev/null)

    if [ "$got" != "$cod" ]; then
        fallo "$nombre" "codigo $cod" "codigo $got — $body"
        return
    fi
    if [ -n "$trozo" ] && ! grep -qF "$trozo" "$TMP/body"; then
        fallo "$nombre" "que contenga '$trozo'" "$body"
        return
    fi
    ok "$nombre"
}

# no_compila <nombre> <fichero> <subcadena-del-error>
no_compila() {
    local nombre="$1" fich="$2" trozo="$3"
    local salida
    salida=$("$LUMEN" --check "$fich" 2>&1)
    if [ $? -eq 0 ]; then
        fallo "$nombre" "error de compilacion" "compilo sin quejarse"
        return
    fi
    if ! printf '%s' "$salida" | grep -qF "$trozo"; then
        fallo "$nombre" "error con '$trozo'" "$(printf '%s' "$salida" | head -2)"
        return
    fi
    ok "$nombre"
}

compila() {
    local nombre="$1" fich="$2"
    if "$LUMEN" --check "$fich" > "$TMP/check" 2>&1; then
        ok "$nombre"
    else
        fallo "$nombre" "que compile" "$(head -3 "$TMP/check")"
    fi
}

limpiar() { parar; rm -rf "$TMP"; }
trap limpiar EXIT

# ─── Suites ──────────────────────────────────────────────────────────────────

echo "== lenguaje =="
levantar "$AQUI/casos/lenguaje.lum" || exit 1
comprueba "aritmetica"          GET /aritmetica       200 '"suma":7'
comprueba "cadenas"             GET /cadenas          200 '"mayus":"HOLA"'
comprueba "multilinea sin margen" GET /multilinea     200 '"sql":"SELECT id\nFROM posts"'
comprueba "multilinea de una"   GET /multilinea       200 '"suelta":"en una linea"'
comprueba "multilinea escapes"  GET /multilinea       200 '"escapes":"con \"comillas\""'
comprueba "veracidad"           GET /veracidad        200 '"cero":false'

# JSON tiene que ser UTF-8 (RFC 8259).  Un 0xFF suelto llega desde la red por
# varias vias, y sin sanear producia una respuesta que ni el cliente que la
# mando podia parsear: no falla la peticion, falla quien la recibe.
json_utf8() {
    local nombre="$1"; shift
    curl -sS --max-time 10 -o "$TMP/body" "$@" 2>/dev/null
    if python3 -c "import json,sys; json.load(open(sys.argv[1], encoding='utf-8'))"             "$TMP/body" 2>/dev/null; then
        ok "$nombre"
    else
        fallo "$nombre" "JSON valido en UTF-8" "$(head -c 120 "$TMP/body" | cat -v)"
    fi
}
json_utf8 "utf8 roto en la query"    "http://127.0.0.1:$PUERTO/eco_query?q=a%FFb"
json_utf8 "utf8 roto en la cabecera" -H "$(printf 'X-Prueba: aÿb')"           "http://127.0.0.1:$PUERTO/eco_cabecera"
# Y lo que si es valido tiene que salir INTACTO: sanear no puede estropear texto
# bueno, que es la mitad que de verdad importa.
comprueba "utf8 valido intacto" GET /eco_valido 200 '"eco":"añoñó 🐻 ñ"'
comprueba "sin coercion"        GET /coercion         500 'no se puede sumar'
comprueba "condicional elif"    GET /clasifica/0      200 '"r":"cero"'
comprueba "condicional else"    GET /clasifica/99     200 '"r":"grande"'
comprueba "bucle while"         GET /suma_hasta/10    200 '"total":55'
comprueba "bucle for y break"   GET /pares            200 '"pares":[2,4,6]'
comprueba "operadores"          GET /operadores       200 '"a":2'
comprueba "incremento previo"   GET /incremento       200 '"pre":7'
comprueba "indices"             GET /indices          200 '"l":[99,20,25]'
comprueba "ternario"            GET /ternario/20      200 '"rol":"adulto"'
comprueba "try captura"         GET /captura          200 'division por cero'
comprueba "error sin capturar"  GET /revienta         500 'division por cero'

echo "== rutas y parametros =="
levantar "$AQUI/casos/rutas.lum" || exit 1
comprueba "parametro de ruta"   GET /eco/42           200 '"id":42'
comprueba "query con defecto"   GET /pagina           200 '"page":1'
comprueba "query explicita"     GET '/pagina?page=7'  200 '"page":7'
comprueba "tipo invalido"       GET /eco/abc          400 'parametro invalido'
comprueba "ruta inexistente"    GET /nada             404 'no existe'
comprueba "grupo con prefijo"   GET /api/v1/hola      200 '"v":1'
comprueba "guarda deniega"      GET /admin/panel      403
comprueba "guarda permite"      GET '/admin/panel?k=abre' 200 '"panel":true'
comprueba "manejador 404"       GET /tampoco          404 '"ruta":"/tampoco"'

# ── Matriz de enlace de parametros ──────────────────────────────────────────
# Origen (ruta / query / multipart) x tipo (escalar, File, List<File>) x
# presencia (falta, mal tipado, en dos sitios a la vez).  Ver casos/parametros.lum:
# el bug real fue un parametro de texto SIEMPRE vacio en una ruta con ficheros,
# y solo aparece cuando los dos conviven en la misma ruta -- probar query y
# multipart cada uno por separado, como hacia el resto de la suite, no lo cazaba.
echo "== enlace de parametros =="
levantar "$AQUI/casos/parametros.lum" || exit 1

comprueba "query ausente sin defecto da el cero del tipo" GET /query 200 '"q":""'

comprueba_mp "multipart: texto junto a un fichero" /mp/uno 200 '"titulo":"hola"' \
    -F "f=@$AQUI/casos/parametros.lum;filename=a.txt" -F "titulo=hola"
comprueba_mp "multipart: el fichero tambien llega" /mp/uno 200 '"filename":"a.txt"' \
    -F "f=@$AQUI/casos/parametros.lum;filename=a.txt" -F "titulo=hola"

comprueba_mp "multipart: string"  /mp/tipos 200 '"s":"hola"' \
    -F "f=@$AQUI/casos/parametros.lum;filename=a.txt" -F "s=hola" -F "n=7" -F "b=true"
comprueba_mp "multipart: int"     /mp/tipos 200 '"n":7' \
    -F "f=@$AQUI/casos/parametros.lum;filename=a.txt" -F "s=hola" -F "n=7" -F "b=true"
comprueba_mp "multipart: bool"    /mp/tipos 200 '"b":true' \
    -F "f=@$AQUI/casos/parametros.lum;filename=a.txt" -F "s=hola" -F "n=7" -F "b=true"

comprueba_mp "multipart: texto junto a List<File>" /mp/lista 200 '"album":"vacaciones"' \
    -F "fs=@$AQUI/casos/parametros.lum;filename=a.txt" -F "album=vacaciones"
comprueba_mp "multipart: cuenta los ficheros de la lista" /mp/lista 200 '"n":2' \
    -F "fs=@$AQUI/casos/parametros.lum;filename=a.txt" \
    -F "fs=@$AQUI/casos/parametros.lum;filename=b.txt" -F "album=x"

comprueba_mp "multipart: campo ausente cae al defecto" /mp/defecto 200 '"etiqueta":"sin-etiqueta"' \
    -F "f=@$AQUI/casos/parametros.lum;filename=a.txt"

comprueba_mp "multipart: la query gana al campo del formulario" "/mp/prioridad?origen=query" 200 '"origen":"query"' \
    -F "f=@$AQUI/casos/parametros.lum;filename=a.txt" -F "origen=formulario"

comprueba_mp "multipart: escalar mal tipado da 400" /mp/malo 400 'parametro invalido' \
    -F "f=@$AQUI/casos/parametros.lum;filename=a.txt" -F "n=no-es-un-numero"

echo "== clases y validacion =="
levantar "$AQUI/casos/clases.lum" || exit 1
comprueba "cuerpo valido"       POST /alta 201 '"creado":"Ana"' '{"nombre":"Ana","edad":30}'
comprueba "campo obligatorio"   POST /alta 422 'edad: obligatorio' '{"nombre":"Ana"}'
comprueba "tipo equivocado"     POST /alta 422 'se esperaba int' '{"nombre":"Ana","edad":"30"}'
comprueba "regla incumplida"    POST /alta 422 'mayor de edad' '{"nombre":"Ana","edad":10}'
comprueba "todos los mensajes"  POST /alta 422 'nombre: obligatorio' '{"nombre":"","edad":10}'
comprueba "json invalido"       POST /alta 400 'JSON invalido' '{roto'
comprueba "mensajes en on error" POST /alta 422 '"cuantos":2' '{"nombre":"","edad":10}'
comprueba "constructor"         GET /punto/3/4        200 '"cuadrado":25'
comprueba "metodo con defecto"  GET /etiqueta/1/2     200 '"otra":"Q(1,2)"'
comprueba "funcion de usuario"  GET /doble/21         200 '"r":42'
comprueba "recursion"           GET /factorial/5      200 '"r":120'
comprueba "tope de recursion"   GET /infinita         500 'demasiada recursion'

echo "== sesion y jwt =="
levantar "$AQUI/casos/sesion.lum" || exit 1
comprueba "sin sesion"          GET /quien            200 '"usuario":null'
comprueba "zona protegida"      GET /admin/panel      403
comprueba "cookie falsificada"  GET /quien            200 '"usuario":null'
comprueba "jwt ausente"         GET /api/yo           401
comprueba "jwt invalido"        GET /api/yo           401

echo "== base de datos =="
rm -f "$TMP/pruebas.db"
levantar "$AQUI/casos/datos.lum" || exit 1
comprueba "crear tabla"         GET  /crear           200 '"ok":true'
comprueba "insertar"            POST /alta/ana        201 '"id":1'
comprueba "insertar otro"       POST /alta/bob        201 '"id":2'
comprueba "listar"              GET  /todos           200 '"nombre":"ana"'
comprueba "buscar por id"       GET  /uno/1           200 '"nombre":"ana"'
comprueba "no encontrado"       GET  /uno/99          404
comprueba "inyeccion sql"       GET  "/busca?q=ana'%20OR%20'1'='1" 200 '"encontrados":0'
comprueba "transaccion"         POST /transfiere      200 '"ok":true'
comprueba "saldos tras commit"  GET  /saldos          200 '"saldo":70'
comprueba "rollback"            POST /deshace         200 '"deshecho":true'
comprueba "saldos tras rollback" GET /saldos          200 '"saldo":70'
comprueba "error del motor"     GET  /malo            200 'no such table'

echo "== errores de compilacion =="
compila    "los ejemplos del repo compilan" "$AQUI/casos/lenguaje.lum"
no_compila "patron sin parametro"  "$AQUI/casos/malos/patron.lum"   "ningun parametro lo recoge"
no_compila "await que falta"       "$AQUI/casos/malos/await.lum"    "es asincrono"
no_compila "objeto fuera de sitio" "$AQUI/casos/malos/sse.lum"      "solo existe dentro de una ruta sse"
no_compila "ws sin origins"        "$AQUI/casos/malos/ws.lum"       "necesita origins"
no_compila "campo inexistente"     "$AQUI/casos/malos/validate.lum" "no esta declarada"
no_compila "metodo inexistente"    "$AQUI/casos/malos/metodo.lum"   "no tiene un metodo"
no_compila "metodo de un string"   "$AQUI/casos/malos/metodo_tipo.lum" "no tienen el metodo"
no_compila "campo de una clase"    "$AQUI/casos/malos/campo_tipo.lum"  "no tiene un campo"
no_compila "modulo sin importar"   "$AQUI/casos/malos/import.lum"   "falta 'import sqlite'"
# Los tipos de las expresiones se comprueban en EJECUCION: el compilador
# verifica nombres, aridad, contexto, y los metodos y campos de un receptor
# cuyo tipo conoce -- pero no que `s - 1` cuadre.
# Eso ya lo cubre la prueba "sin coercion" de la suite de lenguaje.

# ─── Resumen ─────────────────────────────────────────────────────────────────

echo
if [ "$fallidas" -eq 0 ]; then
    verde "$pasadas pruebas, todas pasan"
    exit 0
fi
rojo "$pasadas pasan, $fallidas fallan"
exit 1
