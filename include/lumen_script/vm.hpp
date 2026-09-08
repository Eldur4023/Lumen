#pragma once
#include <string>
#include <vector>

#include "bytecode.hpp"
#include "native_abi.hpp"
#include "natives.hpp"
#include "value.hpp"

namespace lumen_script {

// Maquina virtual de Lumen Script.
//
// La pila de operandos y las variables locales viven aqui, no en la pila de
// C++.  Por eso un handler puede detenerse a mitad: `run()` devuelve
// Suspended con la operacion pendiente, quien lo llama hace el co_await de
// verdad sobre el motor, y `resume()` continua justo donde lo dejo.
//
// El VM no es una corrutina; el handler que lo conduce si lo es.  Esa division
// es lo que evita tener que reimplementar un planificador dentro del VM.
//
// Una instancia por peticion en vuelo, alojada en el marco de la corrutina del
// handler: dos VM suspendidos a la vez no pueden pisarse el estado.
class VM {
public:
    enum class Status { Done, Error, Suspended };

    struct Result {
        Status      status = Status::Done;
        Value       value;          // Done: valor devuelto por el handler
        std::string error;          // Error: motivo
        SourceLoc   error_loc;

        // Suspended: que hay que esperar antes de reanudar.
        int                await_id = -1;
        std::vector<Value> await_args;
    };

    // Arranca `chunk` con `params` en las primeras ranuras.  `functions` es la
    // tabla de funciones de usuario del modulo; puede ser nula si no hay.
    //
    // `native` (ver native_abi.hpp), indexado igual que `functions` (por
    // FnSig::index): un hueco distinto de nullptr en `native->funcs` desvia
    // esa llamada entera a codigo nativo compilado, sin pasar por bytecode.
    // Puede ser nulo (el caso normal, sin --native) o mas corto que
    // `functions` (las funciones sin entrada nunca se compilaron a nativo):
    // un indice fuera de rango se trata igual que un hueco vacio.
    Result start(const Chunk& chunk, std::vector<Value> params, NativeCtx& ctx,
                 const FunctionTable* functions = nullptr,
                 const NativeDispatch* native = nullptr);

    // Continua tras una suspension, dejando `awaited` como valor de la
    // expresion `await`.
    Result resume(Value awaited, NativeCtx& ctx);

    // Tope de pasos por handler: corta un bucle infinito en un .lum en vez de
    // dejar clavado un hilo del event loop, que se llevaria por delante todas
    // las conexiones de ese core.  Se reinicia en cada suspension, porque un
    // bucle de SSE legitimo puede correr durante horas.
    static constexpr long long kStepLimit = 50'000'000;

    // Tope de anidamiento de llamadas.  Una recursion sin caso base tiene que
    // dar un error del lenguaje, no agotar la memoria del proceso.
    static constexpr size_t kMaxFrames = 200;

private:
    // Un marco por llamada en curso.  Locales y pila viven en vectores unicos
    // con una base por marco: asi suspender y reanudar es conservar dos
    // vectores, sin importar a que profundidad de llamada se detuvo.
    struct Frame {
        const Chunk* chunk       = nullptr;
        size_t       pc          = 0;
        size_t       locals_base = 0;
        size_t       stack_base  = 0;
    };

    std::vector<Frame> frames_;
    std::vector<Value> stack_;
    std::vector<Value> locals_;
    const FunctionTable*  functions_ = nullptr;
    const NativeDispatch* native_    = nullptr;

    Result execute(NativeCtx& ctx);
    Result run_until_error(NativeCtx& ctx);

    void  push(Value v) { stack_.push_back(std::move(v)); }
    Value pop()         { Value v = std::move(stack_.back()); stack_.pop_back(); return v; }
};

} // namespace lumen_script
