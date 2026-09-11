#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ast.hpp"
#include "emitter.hpp"
#include "native_abi.hpp"

#include <lumen/task.hpp>

namespace lumen {
class Request;
class Response;
} // namespace lumen

namespace lumen_script {

// Fase 2 de --native, la mitad que faltaba: "invocar el compilador de C++
// desde el propio lumen (no solo desde una prueba) y enlazar el resultado"
// (COMPILACION-NATIVA.md). native_gen.cpp ya sabe generar el texto C++ de
// una funcion; esto lo ensambla, invoca `g++` de verdad como subproceso,
// carga la biblioteca resultante con dlopen(), y deja resueltos los punteros
// de invocacion (CompiledFn, ver native_abi.hpp) que la VM puede llamar
// directamente.
//
// No es todo o nada: cada funcion (fn) del programa se intenta por separado
// -- generar_funcion_nativa() ya decide cuales quedan fuera (contenedores,
// clases, await...) -- y las que no se pudieron compilar sencillamente no
// aparecen aqui; quien las llame seguira sirviendolas con bytecode. Eso es
// el modo mixto que describe el documento, aplicado a funciones sueltas
// porque esta fase no llega todavia a rutas.
class NativeModule {
public:
    NativeModule() = default;
    ~NativeModule();
    NativeModule(NativeModule&& o) noexcept;
    NativeModule& operator=(NativeModule&& o) noexcept;
    NativeModule(const NativeModule&)            = delete;
    NativeModule& operator=(const NativeModule&) = delete;

    // Indexado por FnSig::index, igual que FunctionTable. Un hueco a nullptr
    // significa "esta funcion se sigue sirviendo con bytecode".
    std::vector<CompiledFn> por_indice;

    // Simbolo fijo "lumen_native_error_message" (ver native_abi.hpp): nulo
    // solo si por_indice esta vacio de verdad (no deberia pasar en un
    // NativeModule ya construido, pero por si acaso).
    ErrorMessageFn error_message = nullptr;

    // Fase 4: una ruta nunca cruza la ABI fija (NativeValue) -- nadie la
    // llama desde bytecode, solo build_routes() en C++ normal, asi que el
    // simbolo que exporta tiene la firma real que necesita (ver
    // generar_ruta_nativa en native_gen.hpp), no la generica de CompiledFn.
    // Indexado por posicion en Program::routes, igual que rutas_por_indice
    // aqui debajo -- un hueco a nullptr significa "esta ruta se sigue
    // sirviendo con bytecode".
    using RouteFn = void (*)(lumen::Request&, lumen::Response&);
    std::vector<RouteFn> rutas_por_indice;

    // Fase 5: una ruta cuyo cuerpo usa `await` (hoy: solo `await
    // sleep(ms)`, ver Comprobador::usa_await()) se genera como una
    // corrutina real -- mismo indexado que rutas_por_indice, pero una ruta
    // dada esta en UNA sola de las dos tablas nunca en las dos (ver
    // RutaNativa::asincrona).
    using RouteFnAsync = lumen::Task<void> (*)(lumen::Request&, lumen::Response&);
    std::vector<RouteFnAsync> rutas_async_por_indice;

    size_t compiladas() const;
    size_t rutas_compiladas() const;

    // Vista liviana para VM::start() -- ver el comentario de NativeDispatch
    // en native_abi.hpp.
    NativeDispatch dispatch() const { return {&por_indice, error_message}; }

private:
    friend std::unique_ptr<NativeModule> compilar_nativo(const Program&, const FunctionSigs&,
                                                          const ClassSigs&,
                                                          const std::filesystem::path&,
                                                          std::string&);
    void* handle_ = nullptr;
};

// Genera, compila con el compilador de C++ del sistema, y carga como
// biblioteca compartida las funciones de `prog` (sueltas, y metodos/
// constructores de `clases`) que native_gen.cpp sabe representar.
// `cache_dir` es donde queda el .cpp/.so generados (se crea si hace falta)
// -- hoy siempre se regenera; el cacheado por hash de fuentes que describe
// la seccion 11 del documento queda para cuando de verdad haga falta.
//
// Esto NUNCA tumba la compilacion del modulo -- a diferencia del resto del
// compilador, un fallo aqui (falta `g++`, un error de enlazado, dlopen sin
// suerte) no es un DiagnosticBag: es una degradacion a bytecode para todo el
// modulo, con el motivo en `aviso` (vacio si no hubo problema) para que
// quien llama lo imprima como advertencia, nunca en silencio. Devuelve
// nullptr tanto si nada se pudo generar (no es un error, `aviso` queda
// vacio) como si `g++`/dlopen fallaron.
std::unique_ptr<NativeModule> compilar_nativo(const Program& prog, const FunctionSigs& sigs,
                                              const ClassSigs& clases,
                                              const std::filesystem::path& cache_dir,
                                              std::string& aviso);

} // namespace lumen_script
