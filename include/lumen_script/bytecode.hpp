#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "token.hpp"
#include "value.hpp"

namespace lumen_script {

// VM instructions.
//
// Its own operand stack, not the C++ one: that is what will allow suspending a
// handler halfway through an `await` without losing the state.  The `await`
// itself comes later; the stack design already accounts for it.
enum class Op : uint8_t {
    Const,        // operand = index into the constant pool
    LoadLocal,    // operand = slot
    StoreLocal,   // operand = slot
    Pop,

    Add, Sub, Mul, Div, Mod, Neg,
    Eq, Ne, Lt, Le, Gt, Ge,
    Not,

    // A collapsed chain of '+': operand = how many values off the top are joined.
    //
    // "a" + str(n) + "b" with separate Adds creates a box and a buffer for every
    // '+', and throws away all but the last.  Here the total is measured once,
    // reserved in one go, and only one box is born.
    //
    // It carries a guard like the Int variants: if any of the values is not a
    // string, it folds with the same generic Add, so the result and the error
    // are identical to what the uncollapsed chain of '+' would give.
    ConcatN,

    // Variants with both sides declared `int`.  Lumen Script is statically
    // typed, so what the generic VM works out on every pass is known here once:
    // they are the same operation without the cascade of type checks.
    // comprobaciones de type.
    //
    // The declared type is not enforced on assignment, so they carry a guard:
    // if the values are not really integers, they fall to the generic path and
    // the program behaves the same, with the same error message.
    AddInt, SubInt, MulInt,
    LtInt, LeInt, GtInt, GeInt,

    Jump,         // operand = destino absoluto
    JumpIfFalse,  // same; consumes the top
    JumpIfFalsePeek,  // same, but leaves the top (and/or short-circuit)
    JumpIfTruePeek,

    MakeList,     // operand = number de elementos
    MakeDict,     // operand = number de pares
    GetIndex,
    SetIndex,     // pushes container, index and value; leaves the value
    IterList,     // turns the top into the list to walk
    GetMember,    // operand = index of the constant holding the name
    SetMember,    // same; pushes object and value, and leaves the value

    CallFunction, // operand = (indice-de-funcion << 8) | argc
    CallMethod,   // operand = (name-constant-index << 8) | argc
    CallNative,   // operand = (id << 8) | argc
    CallAsync,    // same, but suspends: the driver does the real co_await
    Return,       // returns the top
    ReturnNull,
};

struct Instr {
    Op        op;
    uint32_t  operand = 0;
    SourceLoc loc;      // so a runtime error can be located
};

// A `try:` covered by its `catch:`.
//
// Resolved by range at compile time and not by a runtime stack: that way a
// `return`, a `break` or a `continue` leaving the try cannot leave a handler
// dangling that would catch a later error.
struct TryRange {
    size_t begin    = 0;   // first protected instruction
    size_t end      = 0;   // first instruction ALREADY outside the try
    size_t catch_pc = 0;
};

// The code of a handler, already compiled.
struct Chunk {
    std::vector<Instr> code;
    // If false, the handler cannot stop and can run on a VM reused per thread
    // instead of one per request.
    bool               has_await = false;
    std::vector<Value>    constants;
    std::vector<TryRange> try_ranges;
    int                   num_locals = 0;

    // Slot names, only for error messages.
    std::vector<std::string> local_names;

    uint32_t add_constant(Value v) {
        constants.push_back(std::move(v));
        return static_cast<uint32_t>(constants.size() - 1);
    }

    size_t emit(Op op, SourceLoc loc, uint32_t operand = 0) {
        code.push_back(Instr{op, operand, loc});
        return code.size() - 1;
    }

    // Jumps are emitted without a target and patched when the block closes.
    void patch(size_t at, size_t target) {
        code[at].operand = static_cast<uint32_t>(target);
    }
    size_t here() const { return code.size(); }
};

// The compiled user functions, indexed by declaration order.  The emitter
// resolves the name to an index, so the VM never looks one up by name.
using FunctionTable = std::vector<std::shared_ptr<Chunk>>;

const char* op_name(Op op);

} // namespace lumen_script
