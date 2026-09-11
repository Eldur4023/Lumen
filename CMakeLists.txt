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
    message(FATAL_ERROR "Lumen requires Linux: it uses epoll, sendfile(2) and SO_REUSEPORT.")
endif()

option(LUMEN_IO_URING "io_uring backend instead of epoll (Linux 5.1+)" OFF)
option(LUMEN_EXAMPLES "Build the examples in examples/"                 ON)

# Manual fuzzing harnesses -- see fuzz/chaos.hpp for why libFuzzer is not used
# (it needs clang; this toolchain is gcc).  Off by default: it is not part of the
# normal build or of ctest, it is launched by hand.
option(LUMEN_FUZZ "Build the fuzzing harnesses in fuzz/" OFF)

# Persistence modules.  Each one builds only if its client is present: `import
# sqlite` in a binary without it gives a clear error when compiling the .lum, not
# a strange failure in production.
option(LUMEN_SQLITE   "Modulo sqlite"                                   ON)
option(LUMEN_POSTGRES "Modulo postgres (libpq)"                         ON)
option(LUMEN_MYSQL    "Modulo mysql (libmysqlclient)"                   ON)

# ── System dependencies ───────────────────────────────────────────────────────
# Lumen links neither OpenSSL, nghttp2 nor zlib: TLS, HTTP/2 and compression are
# the reverse proxy's business.  Only threads are needed.

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ── jemalloc ──────────────────────────────────────────────────────────────────
# With several event loops and a database pool, glibc's malloc becomes the
# bottleneck: its arenas serialize and the mutex eats 20% of the time.  Measured
# on large JSON responses, 8 cores:
#
#   /articulos?size=100 (57 KB)   18.782 -> 29.802 req/s   +59%
#   /articulos?page=3   (11 KB)   49.051 -> 65.307 req/s   +33%
#
# It is optional: without it the build proceeds and glibc's is used.
option(LUMEN_JEMALLOC "Link jemalloc if available" ON)
set(LUMEN_ALLOC "")
if(LUMEN_JEMALLOC)
    find_library(JEMALLOC_LIB NAMES jemalloc)
    if(JEMALLOC_LIB)
        set(LUMEN_ALLOC ${JEMALLOC_LIB})
        message(STATUS "allocator: jemalloc (${JEMALLOC_LIB})")
    else()
        message(STATUS "allocator: glibc — install libjemalloc-dev for the real one")
    endif()
endif()

# No dependencies to fetch from the network: llhttp is vendored and everything
# else —JSON, templates, cryptography— is built in-house.
#
# ── third_party (vendored, no network during cmake) ───────────────────────────
add_library(lumen_llhttp STATIC
    third_party/llhttp/src/api.c
    third_party/llhttp/src/http.c
    third_party/llhttp/src/llhttp.c
)
target_include_directories(lumen_llhttp PUBLIC third_party/llhttp/include)
set_target_properties(lumen_llhttp PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ── Main library ──────────────────────────────────────────────────────────────
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

# ── Lumen Script: the language frontend ───────────────────────────────────────
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
    src/lumen_script/template.cpp
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

# --native (phase 4/5, routes): native_build.cpp invokes g++ as a subprocess
# to compile the .cpp a route generates -- it needs to know, at runtime of the
# `lumen` binary, where the project's own headers are (for
# #include <lumen/response.hpp>, <lumen_script/value.hpp>...) and where the
# already-compiled liblumen_script.a/liblumen.a ended up. liblumen_script is
# needed for Value::write_json (not inline); liblumen for
# lumen::core::EpollLoop::post -- needed by any route with `await`, because
# the Task<void> a coroutine generates schedules itself to self-destruct
# there once it finishes (see Task<void>::promise_type::FinalAwaitable in
# task.hpp) -- found in practice (undefined symbol loading the .so) testing
# the first route with `await sleep(...)`, not guessed beforehand.
# There is no way for an already-linked binary to guess these paths on its
# own: they are baked in here, at CMake's own compile time.
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
        message(STATUS "sqlite module: enabled")
    else()
        message(STATUS "sqlite module: SKIPPED (libsqlite3-dev missing)")
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
        message(STATUS "postgres module: enabled")
    else()
        message(STATUS "postgres module: SKIPPED (libpq-dev missing)")
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
        message(STATUS "mysql module: enabled")
    else()
        message(STATUS "mysql module: SKIPPED (libmysqlclient-dev missing)")
    endif()
endif()
target_compile_options(lumen_script PRIVATE -Wall -Wextra)

# The Lumen binary.
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

# Frontend development tools.
add_executable(dump_tokens src/lumen_script/tools/dump_tokens.cpp)
target_link_libraries(dump_tokens PRIVATE lumen_script)

add_executable(crypto_check src/lumen_script/tools/crypto_check.cpp)
target_link_libraries(crypto_check PRIVATE lumen_script)

# ── Tests ─────────────────────────────────────────────────────────────────────
# The suite is a script that drives the binary over the socket, the way it is
# used.  It links nothing from the project, so it tests what gets deployed and
# not an instrumented version of it.
enable_testing()

# Templates are tested separately and in process: compiling and rendering do not
# go through the socket, so the regression script cannot reach them.
add_executable(test_templates tests/templates.cpp)
target_link_libraries(test_templates PRIVATE lumen_script)
add_test(NAME templates COMMAND test_templates)

# --native phase 1 (COMPILACION-NATIVA.md): compares Emitter::check_expr/
# check_call against the real compilation, expression by expression. It lives
# here, not in tests/run_tests.sh, because it calls the library directly
# instead of the binary: it is a test of an internal piece not wired up yet.
add_executable(test_check_expr_shadow tests/check_expr_shadow.cpp)
target_link_libraries(test_check_expr_shadow PRIVATE lumen_script)
add_test(NAME check_expr_shadow COMMAND test_check_expr_shadow)

add_executable(test_check_stmt_shadow tests/check_stmt_shadow.cpp)
target_link_libraries(test_check_stmt_shadow PRIVATE lumen_script)
add_test(NAME check_stmt_shadow COMMAND test_check_stmt_shadow)

# The other half of phase 1: not just that check_expr/check_stmt reproduce
# the same diagnostics (above), but that the emitter consuming the IR
# produces bytecode that BEHAVES the same when actually run on the VM.
add_executable(test_emit_ir_shadow tests/emit_ir_shadow.cpp)
target_link_libraries(test_emit_ir_shadow PRIVATE lumen_script)
add_test(NAME emit_ir_shadow COMMAND test_emit_ir_shadow)

# --native phase 2: generates real C++ from the IR, compiles it with the
# system compiler (invokes `g++` as a subprocess) and compares its execution
# against the VM -- the phase's acceptance criterion.
add_executable(test_native_gen_shadow tests/native_gen_shadow.cpp)
target_link_libraries(test_native_gen_shadow PRIVATE lumen_script)
add_test(NAME native_gen_shadow COMMAND test_native_gen_shadow)

# The other half: compile_native() for real (invokes g++, dlopen, dlsym) and
# dispatch inside the VM (VM::start with the native table wired up) give the
# same result as bytecode -- see the comment in tests/native_build_shadow.cpp.
add_executable(test_native_build_shadow tests/native_build_shadow.cpp)
target_link_libraries(test_native_build_shadow PRIVATE lumen_script)
add_test(NAME native_build_shadow COMMAND test_native_build_shadow)

# User classes: generates the C++ for a class and its methods directly
# (without going through compile_native(), which today has nothing to offer
# a class-only program -- see the comment in tests/native_class_shadow.cpp)
# and compares its execution against the VM.
add_executable(test_native_class_shadow tests/native_class_shadow.cpp)
target_link_libraries(test_native_class_shadow PRIVATE lumen_script)
add_test(NAME native_class_shadow COMMAND test_native_class_shadow)

# Phase 4: one HTTP route compiled to native, end to end, through the real
# compile() (parsing, build_routes, compile_native) -- not a Program built by
# hand -- compared against the same route served by bytecode. Needs g++ to
# find <lumen/response.hpp> and link liblumen_script.a: same macros
# native_build.cpp already uses (see the comment on LUMEN_NATIVE_INCLUDE_DIR
# on the lumen_script target), so there is no need to repeat them here.
add_executable(test_native_route_shadow tests/native_route_shadow.cpp)
target_link_libraries(test_native_route_shadow PRIVATE lumen_script)
# Same reason as lumen-bin above, and the same kind of symptom found the same
# way (by testing for real, not guessed beforehand): state.incr()/state.get()
# (phase 5.9) resolve against SharedState::instance(), a function-local
# `static` in natives.cpp -- without -rdynamic, the .so that compile_native()
# dlopen()s here carries its OWN copy (liblumen_script.a linked again inside
# the .so), so a bytecode state.incr() and a --native one, in the SAME test,
# incremented DIFFERENT counters -- this test caught it (sequence 1,2,1,1,2
# instead of 1,2,3,3,3) before this comment was written.
target_link_options(test_native_route_shadow PRIVATE -rdynamic)
add_test(NAME native_route_shadow COMMAND test_native_route_shadow)

# The postgres placeholder translator is a pure function: it is tested without a
# server by including the driver's .cpp, to test THE code and not a copy.
if(LUMEN_POSTGRES AND PQ_INC AND PQ_LIB)
    add_executable(test_placeholders tests/placeholders.cpp)
    target_include_directories(test_placeholders PRIVATE ${PQ_INC})
    target_link_libraries(test_placeholders PRIVATE lumen_script ${PQ_LIB})
    add_test(NAME placeholders COMMAND test_placeholders)
endif()

add_test(NAME regression
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:lumen-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(regression PROPERTIES TIMEOUT 300)

# The mysql module needs a server, so its suite skips itself —code 77— when
# there is none, instead of turning a machine where it does not apply red.
# The instructions for setting the environment up are in the script's header.
add_test(NAME mysql
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mysql.sh $<TARGET_FILE:lumen-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(mysql PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# The sqlite type battery needs no server —the schema is created by the .lum
# itself— so it always runs.
add_test(NAME sqlite
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_sqlite.sh $<TARGET_FILE:lumen-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(sqlite PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# Same for postgres.
add_test(NAME postgres
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_postgres.sh $<TARGET_FILE:lumen-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(postgres PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# ── Fuzzing ───────────────────────────────────────────────────────────────────
# No sanitizer flags here on purpose: for ASan/UBSan to be worth anything they
# have to instrument lumen_script/lumen too, not just these two .cpp files, so
# they go in CMAKE_CXX_FLAGS for the whole configuration -- see fuzz/README.md.
# Each case runs in a child (fuzz/chaos.hpp): a sanitizer abort takes down
# only that child, the campaign carries on.
if(LUMEN_FUZZ)
    add_executable(fuzz_language fuzz/fuzz_language.cpp)
    target_link_libraries(fuzz_language PRIVATE lumen_script)

    add_executable(fuzz_http fuzz/fuzz_http.cpp)
    target_include_directories(fuzz_http PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(fuzz_http PRIVATE lumen_script)
endif()

# ── Examples ──────────────────────────────────────────────────────────────────
if(LUMEN_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples)
    file(GLOB LUMEN_EXAMPLE_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp)
    foreach(src ${LUMEN_EXAMPLE_SRCS})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE lumen)
    endforeach()
endif()
