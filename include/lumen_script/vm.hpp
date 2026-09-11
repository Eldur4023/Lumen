#pragma once
#include <string>
#include <vector>

#include "bytecode.hpp"
#include "native_abi.hpp"
#include "natives.hpp"
#include "value.hpp"

namespace lumen_script {

// Virtual machine of Lumen Script.
//
// The operand stack and the local variables live here, not on the C++ stack.
// That is why a handler can stop halfway: `run()` returns Suspended with the
// pending operation, the caller does the real co_await on the engine, and
// `resume()` picks up exactly where it left off.
//
// The VM is not a coroutine; the handler driving it is.  That split is what
// avoids reimplementing a scheduler inside the VM.
//
// One instance per in-flight request, held in the handler's coroutine frame:
// two suspended VMs cannot tread on each other's state.
class VM {
public:
    enum class Status { Done, Error, Suspended };

    struct Result {
        Status      status = Status::Done;
        Value       value;          // Done: value returned by the handler
        std::string error;          // Error: motivo
        SourceLoc   error_loc;

        // Suspended: what to wait for before resuming.
        int                await_id = -1;
        std::vector<Value> await_args;
    };

    // Starts `chunk` with `params` in the first slots. `functions` is the
    // module's user function table; it may be null if there is none.
    //
    // `native` (see native_abi.hpp), indexed the same way as `functions` (by
    // FnSig::index): a slot in `native->funcs` that isn't nullptr redirects
    // that whole call to compiled native code, bypassing bytecode entirely.
    // May be null (the normal case, without --native) or shorter than
    // `functions` (functions with no entry were never compiled to native):
    // an out-of-range index is treated the same as an empty slot.
    Result start(const Chunk& chunk, std::vector<Value> params, NativeCtx& ctx,
                 const FunctionTable* functions = nullptr,
                 const NativeDispatch* native = nullptr);

    // Continues after a suspension, leaving `awaited` as the value of the
    // `await` expression.
    Result resume(Value awaited, NativeCtx& ctx);

    // Step cap per handler: cuts an infinite loop in a .lum instead of pinning
    // an event loop thread, which would take down every connection on that
    // core.  It resets on every suspension, because a legitimate SSE loop can
    // run for hours.
    static constexpr long long kStepLimit = 50'000'000;

    // Call nesting cap.  Recursion without a base case has to give a language
    // error, not exhaust the process memory.
    static constexpr size_t kMaxFrames = 200;

private:
    // One frame per call in progress.  Locals and stack live in single vectors
    // with a base per frame: that way suspending and resuming is keeping two
    // vectors, no matter how deep the call was when it stopped.
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
