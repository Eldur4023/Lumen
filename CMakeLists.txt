cmake_minimum_required(VERSION 3.20)
project(lumen CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# --native (Fase 4, rutas): el .so que genera compilar_nativo() para una ruta
# enlaza liblumen_script.a de verdad (necesita Value::write_json, no inline en
# el header -- ver COMPILACION-NATIVA.md) desde una biblioteca COMPARTIDA. Un
# .a compilado sin -fPIC no se puede meter dentro de un .so -- de ahi esto
# global, no solo en el target de lumen_script: mas simple que marcar cada
# dependencia transitiva (lumen, lumen_llhttp) una por una, y el coste (PIC en
# el binario `lumen` mismo, que no lo necesitaba) es insignificante frente a
# tener que recompilar bajo demanda.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Lumen requiere Linux: usa epoll, sendfile(2) y SO_REUSEPORT.")
endif()

option(LUMEN_IO_URING "Backend io_uring en lugar de epoll (Linux 5.1+)" OFF)
option(LUMEN_EXAMPLES "Compilar los ejemplos de examples/"              ON)

# Arneses de fuzzing manuales -- ver fuzz/chaos.hpp para el motivo de no usar
# libFuzzer (necesita clang; este toolchain es gcc).  Apagado por defecto: no
# es parte de la build normal ni de ctest, se lanza a mano.
option(LUMEN_FUZZ "Compilar los arneses de fuzzing de fuzz/" OFF)

# Modulos de persistencia.  Cada uno se compila solo si su cliente esta
# presente: `import sqlite` en un binario sin el da un error claro al compilar
# el .lum, no un fallo raro en produccion.
option(LUMEN_SQLITE   "Modulo sqlite"                                   ON)
option(LUMEN_POSTGRES "Modulo postgres (libpq)"                         ON)
option(LUMEN_MYSQL    "Modulo mysql (libmysqlclient)"                   ON)

# ── Dependencias del sistema ──────────────────────────────────────────────────
# Lumen 2.0 no enlaza OpenSSL, nghttp2 ni zlib: TLS, HTTP/2 y compresion son
# cosa del reverse proxy.  Solo hacen falta hilos.

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ── jemalloc ──────────────────────────────────────────────────────────────────
# Con varios event loops y un pool de base de datos, el malloc de glibc se
# convierte en el cuello de botella: sus arenas se serializan y el mutex se come
# el 20% del tiempo.  Medido sobre respuestas JSON grandes, 8 cores:
#
#   /articulos?size=100 (57 KB)   18.782 -> 29.802 req/s   +59%
#   /articulos?page=3   (11 KB)   49.051 -> 65.307 req/s   +33%
#
# Es opcional: si no esta, se compila igual y se sigue usando el de glibc.
option(LUMEN_JEMALLOC "Enlazar jemalloc si esta disponible" ON)
set(LUMEN_ALLOC "")
if(LUMEN_JEMALLOC)
    find_library(JEMALLOC_LIB NAMES jemalloc)
    if(JEMALLOC_LIB)
        set(LUMEN_ALLOC ${JEMALLOC_LIB})
        message(STATUS "asignador: jemalloc (${JEMALLOC_LIB})")
    else()
        message(STATUS "asignador: glibc — instala libjemalloc-dev para el de verdad")
    endif()
endif()

# Sin dependencias que traer de la red: llhttp esta vendorizado y todo lo demas
# —JSON, plantillas, criptografia— es de casa.
#
# ── third_party (vendorizado, sin red durante cmake) ──────────────────────────
add_library(lumen_llhttp STATIC
    third_party/llhttp/src/api.c
    third_party/llhttp/src/http.c
    third_party/llhttp/src/llhttp.c
)
target_include_directories(lumen_llhttp PUBLIC third_party/llhttp/include)
set_target_properties(lumen_llhttp PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ── Biblioteca principal ──────────────────────────────────────────────────────
set(LUMEN_SOURCES
    src/app.cpp
    src/router.cpp
    src/core/event_loop.cpp
    src/core/tcp_server.cpp
    src/http/http_parser.cpp
    src/http/http_connection.cpp
)

if(LUMEN_IO_URING)
    list(APPEND LUMEN_SOURCES src/core/io_uring_loop.cpp)
endif()

add_library(lumen STATIC ${LUMEN_SOURCES})

target_include_directories(lumen PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(lumen PUBLIC
    lumen_llhttp
    Threads::Threads
    ${LUMEN_ALLOC}
)

if(LUMEN_IO_URING)
    find_library(URING_LIB uring REQUIRED)
    target_compile_definitions(lumen PUBLIC LUMEN_IO_URING)
    target_link_libraries(lumen PUBLIC ${URING_LIB})
endif()

target_compile_options(lumen PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ── Lumen Script: frontend del lenguaje ───────────────────────────────────────────────
add_library(lumen_script STATIC
    src/lumen_script/token.cpp
    src/lumen_script/diagnostic.cpp
    src/lumen_script/lexer.cpp
    src/lumen_script/parser.cpp
    src/lumen_script/crypto.cpp
    src/lumen_script/auth.cpp
    src/lumen_script/type.cpp
    src/lumen_script/db.cpp
    src/lumen_script/value.cpp
    src/lumen_script/json_parse.cpp
    src/lumen_script/plantilla.cpp
    src/lumen_script/natives.cpp
    src/lumen_script/emitter.cpp
    src/lumen_script/native_gen.cpp
    src/lumen_script/native_build.cpp
    src/lumen_script/vm.cpp
    src/lumen_script/autotest.cpp
    src/lumen_script/project.cpp
)
target_include_directories(lumen_script PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lumen_script PUBLIC lumen ${CMAKE_DL_LIBS})

# --native (Fase 4/5, rutas): native_build.cpp invoca g++ como subproceso
# para compilar el .cpp que genera una ruta -- necesita saber, en tiempo de
# ejecucion del binario `lumen`, donde estan las cabeceras del propio
# proyecto (para #include <lumen/response.hpp>, <lumen_script/value.hpp>...)
# y donde quedaron liblumen_script.a/liblumen.a ya compiladas. liblumen_script
# hace falta para Value::write_json (no es inline); liblumen para
# lumen::core::EpollLoop::post -- que necesita cualquier ruta con `await`,
# porque el Task<void> que genera una corrutina la agenda para autodestruirse
# ahi cuando termina (ver Task<void>::promise_type::FinalAwaitable en
# task.hpp) -- encontrado en la practica (undefined symbol al cargar el .so)
# probando la primera ruta con `await sleep(...)`, no adivinado de antemano.
# No hay forma de que un binario ya enlazado adivine estas rutas por su
# cuenta: se hornean aqui, en tiempo de compilacion de CMake.
target_compile_definitions(lumen_script PRIVATE
    LUMEN_NATIVE_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include"
    LUMEN_NATIVE_SCRIPT_LIB="$<TARGET_FILE:lumen_script>"
    LUMEN_NATIVE_LIB="$<TARGET_FILE:lumen>")

if(LUMEN_SQLITE)
    find_path(SQLITE3_INC sqlite3.h)
    find_library(SQLITE3_LIB sqlite3)
    if(SQLITE3_INC AND SQLITE3_LIB)
        target_sources(lumen_script PRIVATE src/lumen_script/db_sqlite.cpp)
        target_compile_definitions(lumen_script PRIVATE LUMEN_SQLITE)
        target_include_directories(lumen_script PRIVATE ${SQLITE3_INC})
        target_link_libraries(lumen_script PRIVATE ${SQLITE3_LIB})
        message(STATUS "modulo sqlite: activado")
    else()
        message(STATUS "modulo sqlite: OMITIDO (falta libsqlite3-dev)")
    endif()
endif()

if(LUMEN_POSTGRES)
    find_path(PQ_INC libpq-fe.h PATH_SUFFIXES postgresql)
    find_library(PQ_LIB pq)
    if(PQ_INC AND PQ_LIB)
        target_sources(lumen_script PRIVATE src/lumen_script/db_postgres.cpp)
        target_compile_definitions(lumen_script PRIVATE LUMEN_POSTGRES)
        target_include_directories(lumen_script PRIVATE ${PQ_INC})
        target_link_libraries(lumen_script PRIVATE ${PQ_LIB})
        message(STATUS "modulo postgres: activado")
    else()
        message(STATUS "modulo postgres: OMITIDO (falta libpq-dev)")
    endif()
endif()

if(LUMEN_MYSQL)
    find_path(MYSQL_INC mysql.h PATH_SUFFIXES mysql mariadb)
    find_library(MYSQL_LIB NAMES mysqlclient mariadb)
    if(MYSQL_INC AND MYSQL_LIB)
        target_sources(lumen_script PRIVATE src/lumen_script/db_mysql.cpp)
        target_compile_definitions(lumen_script PRIVATE LUMEN_MYSQL)
        target_include_directories(lumen_script PRIVATE ${MYSQL_INC})
        target_link_libraries(lumen_script PRIVATE ${MYSQL_LIB})
        message(STATUS "modulo mysql: activado")
    else()
        message(STATUS "modulo mysql: OMITIDO (falta libmysqlclient-dev)")
    endif()
endif()
target_compile_options(lumen_script PRIVATE -Wall -Wextra)

# El binario de Lumen 2.0.
add_executable(lumen-bin src/lumen_script/main.cpp)
set_target_properties(lumen-bin PROPERTIES OUTPUT_NAME lumen)
target_link_libraries(lumen-bin PRIVATE lumen_script)
# --native (Fase 5, `await`): sin esto, el .so que dlopen() carga en tiempo
# de ejecucion NO comparte los `inline thread_local` de task.hpp
# (lumen::detail::current_loop/current_token, que HttpConnection::dispatch()
# deja puestos antes de cada peticion) con este ejecutable -- cada uno se
# lleva su propia copia privada, siempre a null en la del .so. Sin loop,
# SleepAwaitable::await_suspend() resume el acto, sin esperar nada: `await
# sleep(200)` en una ruta nativa respondia en ~3ms en vez de ~200ms,
# encontrado probando la primera ruta con `await` de verdad contra HTTP, no
# adivinado de antemano. -rdynamic exporta los simbolos de este binario al
# tabla dinamica para que el .so resuelva contra ELLOS, no cree los suyos.
target_link_options(lumen-bin PRIVATE -rdynamic)

# Herramientas de desarrollo del frontend.
add_executable(dump_tokens src/lumen_script/tools/dump_tokens.cpp)
target_link_libraries(dump_tokens PRIVATE lumen_script)

add_executable(crypto_check src/lumen_script/tools/crypto_check.cpp)
target_link_libraries(crypto_check PRIVATE lumen_script)

# ── Pruebas ───────────────────────────────────────────────────────────────────
# La suite es un script que ejerce el binario por el socket, tal y como se usa.
# No enlaza nada del proyecto, asi que prueba lo que se despliega y no una
# version instrumentada de ello.
enable_testing()

# Las plantillas se prueban aparte y en proceso: compilar y renderizar no pasa
# por el socket, asi que el guion de regresion no llega ahi.
add_executable(prueba_plantillas tests/plantillas.cpp)
target_link_libraries(prueba_plantillas PRIVATE lumen_script)
add_test(NAME plantillas COMMAND prueba_plantillas)

# Fase 1 de --native (COMPILACION-NATIVA.md): compara Emitter::check_expr/
# check_call contra la compilacion real, expresion a expresion. Vive aqui, no
# en tests/run_tests.sh, porque llama a la libreria directamente en vez de al
# binario: es una prueba de una pieza interna que todavia no esta conectada.
add_executable(prueba_check_expr tests/check_expr_shadow.cpp)
target_link_libraries(prueba_check_expr PRIVATE lumen_script)
add_test(NAME check_expr_shadow COMMAND prueba_check_expr)

add_executable(prueba_check_stmt tests/check_stmt_shadow.cpp)
target_link_libraries(prueba_check_stmt PRIVATE lumen_script)
add_test(NAME check_stmt_shadow COMMAND prueba_check_stmt)

# La otra mitad de la fase 1: no solo que check_expr/check_stmt reproducen
# los mismos diagnosticos (arriba), sino que el emisor que consume el IR
# produce bytecode que se COMPORTA igual ejecutado de verdad en el VM.
add_executable(prueba_emit_ir tests/emit_ir_shadow.cpp)
target_link_libraries(prueba_emit_ir PRIVATE lumen_script)
add_test(NAME emit_ir_shadow COMMAND prueba_emit_ir)

# Fase 2 de --native: genera C++ real desde el IR, lo compila con el
# compilador del sistema (invoca `g++` como subproceso) y compara su
# ejecucion contra la VM -- el criterio de aceptacion de la fase.
add_executable(prueba_native_gen tests/native_gen_shadow.cpp)
target_link_libraries(prueba_native_gen PRIVATE lumen_script)
add_test(NAME native_gen_shadow COMMAND prueba_native_gen)

# La otra mitad: compilar_nativo() de verdad (invoca g++, dlopen, dlsym) y el
# despacho dentro de la VM (VM::start con la tabla nativa conectada) dan lo
# mismo que el bytecode -- ver el comentario en tests/native_build_shadow.cpp.
add_executable(prueba_native_build tests/native_build_shadow.cpp)
target_link_libraries(prueba_native_build PRIVATE lumen_script)
add_test(NAME native_build_shadow COMMAND prueba_native_build)

# Clases de usuario: genera el C++ de una clase y sus metodos directamente
# (sin pasar por compilar_nativo(), que hoy no tiene nada que ofrecer para un
# programa solo-clases -- ver el comentario en tests/native_class_shadow.cpp)
# y compara su ejecucion contra la VM.
add_executable(prueba_native_class tests/native_class_shadow.cpp)
target_link_libraries(prueba_native_class PRIVATE lumen_script)
add_test(NAME native_class_shadow COMMAND prueba_native_class)

# Fase 4: una ruta HTTP compilada a nativo, de punta a punta, a traves de
# compile() de verdad (parseo, build_routes, compilar_nativo) -- no un
# Program armado a mano -- comparada contra la misma ruta servida por
# bytecode. Necesita que g++ pueda encontrar <lumen/response.hpp> y enlazar
# liblumen_script.a: mismas macros que ya usa native_build.cpp (ver el
# comentario sobre LUMEN_NATIVE_INCLUDE_DIR en el target lumen_script), asi
# que ni falta que hace repetirlas aqui.
add_executable(prueba_native_route tests/native_route_shadow.cpp)
target_link_libraries(prueba_native_route PRIVATE lumen_script)
# Mismo motivo que lumen-bin mas arriba, y la misma clase de sintoma
# encontrada de la misma forma (probando de verdad, no adivinado antes):
# state.incr()/state.get() (Fase 5.9) resuelven contra SharedState::
# instance(), una `static` de funcion en natives.cpp -- sin -rdynamic, el
# .so que compila_nativo() dlopen-ea aqui se lleva su PROPIA copia
# (liblumen_script.a enlazada de nuevo dentro del .so), asi que un
# state.incr() bytecode y uno --native, en la MISMA prueba, incrementaban
# contadores DISTINTOS -- esta prueba lo detecto (secuencia 1,2,1,1,2 en
# vez de 1,2,3,3,3) antes de que este comentario se escribiera.
target_link_options(prueba_native_route PRIVATE -rdynamic)
add_test(NAME native_route_shadow COMMAND prueba_native_route)

# El traductor de marcadores de postgres es una funcion pura: se prueba sin
# servidor incluyendo el .cpp del driver, para probar EL codigo y no una copia.
if(LUMEN_POSTGRES AND PQ_INC AND PQ_LIB)
    add_executable(prueba_marcadores tests/marcadores.cpp)
    target_include_directories(prueba_marcadores PRIVATE ${PQ_INC})
    target_link_libraries(prueba_marcadores PRIVATE lumen_script ${PQ_LIB})
    add_test(NAME marcadores COMMAND prueba_marcadores)
endif()

add_test(NAME regresion
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:lumen-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(regresion PROPERTIES TIMEOUT 300)

# El modulo de mysql necesita un servidor, asi que su suite se salta sola —codigo
# 77— cuando no lo hay, en vez de poner en rojo una maquina donde no aplica.
# Las instrucciones para montar el entorno estan en la cabecera del guion.
add_test(NAME mysql
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mysql.sh $<TARGET_FILE:lumen-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(mysql PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# La bateria de tipos de sqlite no necesita servidor —el esquema lo crea el
# propio .lum— asi que corre siempre.
add_test(NAME sqlite
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_sqlite.sh $<TARGET_FILE:lumen-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(sqlite PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# Idem para postgres.
add_test(NAME postgres
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_postgres.sh $<TARGET_FILE:lumen-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(postgres PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# ── Fuzzing ───────────────────────────────────────────────────────────────────
# Sin flags de sanitizador aqui a proposito: para que ASan/UBSan sirvan de algo
# tienen que instrumentar tambien lumen_script/lumen, no solo estos dos .cpp, asi que
# van en CMAKE_CXX_FLAGS de la configuracion entera -- ver fuzz/README.md.
# Cada caso corre en un hijo (fuzz/chaos.hpp): un abort de sanitizador tumba
# solo ese hijo, la campana sigue.
if(LUMEN_FUZZ)
    add_executable(fuzz_lenguaje fuzz/fuzz_lenguaje.cpp)
    target_link_libraries(fuzz_lenguaje PRIVATE lumen_script)

    add_executable(fuzz_http fuzz/fuzz_http.cpp)
    target_include_directories(fuzz_http PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(fuzz_http PRIVATE lumen_script)
endif()

# ── Ejemplos ──────────────────────────────────────────────────────────────────
if(LUMEN_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples)
    file(GLOB LUMEN_EXAMPLE_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp)
    foreach(src ${LUMEN_EXAMPLE_SRCS})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE lumen)
    endforeach()
endif()
