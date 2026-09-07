#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "token.hpp"
#include "value.hpp"

namespace lumen_script {

// Instrucciones del VM.
//
// Pila de operandos propia, no la de C++: es lo que permitira suspender un
// handler a mitad de `await` sin perder el estado.  El `await` en si llega
// despues; el diseno de la pila ya lo contempla.
enum class Op : uint8_t {
    Const,        // operand = indice en el pool de constantes
    LoadLocal,    // operand = ranura
    StoreLocal,   // operand = ranura
    Pop,

    Add, Sub, Mul, Div, Mod, Neg,
    Eq, Ne, Lt, Le, Gt, Ge,
    Not,

    // Variantes con los dos lados declarados `int`.  Lumen Script es de tipado
    // estatico, asi que el emisor sabe al compilar lo que el VM generico tiene
    // que averiguar en cada vuelta: son la misma operacion sin la cascada de
    // comprobaciones de tipo.
    //
    // El tipo declarado no se impone en la asignacion, asi que llevan guarda:
    // si los valores no son enteros de verdad, caen al camino generico y el
    // programa se comporta igual, con el mismo mensaje de error.
    AddInt, SubInt, MulInt,
    LtInt, LeInt, GtInt, GeInt,

    Jump,         // operand = destino absoluto
    JumpIfFalse,  // idem; consume la cima
    JumpIfFalsePeek,  // idem, pero deja la cima (cortocircuito de and/or)
    JumpIfTruePeek,

    MakeList,     // operand = numero de elementos
    MakeDict,     // operand = numero de pares
    GetIndex,
    SetIndex,     // apila contenedor, indice y valor; deja el valor
    IterList,     // convierte la cima en la lista que se va a recorrer
    GetMember,    // operand = indice de constante con el nombre
    SetMember,    // idem; apila objeto y valor, y deja el valor

    CallFunction, // operand = (indice-de-funcion << 8) | argc
    CallMethod,   // operand = (indice-de-constante-con-el-nombre << 8) | argc
    CallNative,   // operand = (id << 8) | argc
    CallAsync,    // idem, pero suspende: el driver hace el co_await real
    Return,       // devuelve la cima
    ReturnNull,
};

struct Instr {
    Op        op;
    uint32_t  operand = 0;
    SourceLoc loc;      // para poder situar un error de ejecucion
};

// Un `try:` cubierto por su `catch:`.
//
// Se resuelve por rango en compilacion y no por una pila en runtime: asi un
// `return`, un `break` o un `continue` que salgan del try no pueden dejar un
// manejador colgado que atrape un error posterior.
struct TryRange {
    size_t begin    = 0;   // primera instruccion protegida
    size_t end      = 0;   // primera instruccion YA fuera del try
    size_t catch_pc = 0;
};

// El codigo de un handler, ya compilado.
struct Chunk {
    std::vector<Instr> code;
    // Si es false, el handler no puede detenerse y se puede ejecutar sobre un
    // VM reutilizado por hilo en vez de uno por peticion.
    bool               has_await = false;
    std::vector<Value>    constants;
    std::vector<TryRange> try_ranges;
    int                   num_locals = 0;

    // Nombres de las ranuras, solo para mensajes de error.
    std::vector<std::string> local_names;

    uint32_t add_constant(Value v) {
        constants.push_back(std::move(v));
        return static_cast<uint32_t>(constants.size() - 1);
    }

    size_t emit(Op op, SourceLoc loc, uint32_t operand = 0) {
        code.push_back(Instr{op, operand, loc});
        return code.size() - 1;
    }

    // Los saltos se emiten sin destino y se rellenan al cerrar el bloque.
    void patch(size_t at, size_t target) {
        code[at].operand = static_cast<uint32_t>(target);
    }
    size_t here() const { return code.size(); }
};

// Las funciones de usuario compiladas, indexadas por el orden de declaracion.
// El emisor resuelve el nombre a indice, asi que el VM no busca por nombre.
using FunctionTable = std::vector<std::shared_ptr<Chunk>>;

const char* op_name(Op op);

} // namespace lumen_script
