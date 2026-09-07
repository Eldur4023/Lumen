cmake_minimum_required(VERSION 3.20)
project(lumen CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

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
    src/lumen_script/db.cpp
    src/lumen_script/value.cpp
    src/lumen_script/json_parse.cpp
    src/lumen_script/template.cpp
    src/lumen_script/natives.cpp
    src/lumen_script/emitter.cpp
    src/lumen_script/vm.cpp
    src/lumen_script/autotest.cpp
    src/lumen_script/project.cpp
)
target_include_directories(lumen_script PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lumen_script PUBLIC lumen)

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
