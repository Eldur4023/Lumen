#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "bytecode.hpp"
#include "value.hpp"

namespace lumen {
class Request; class Response; class SSEWriter; class WSConnection;
struct MultipartPart;
}

namespace lumen_script {

// Context the VM passes to the builtins: it is the layer-2 bridge, where the
// bytecode reaches the native engine.
// Store shared across ALL the event loop threads.
//
// It is the only shared-state path: every VM has its own stack and heap and
// shares nothing with the others.  That is why it exposes operations and not
// properties — `state.x = state.x + 1` would race the read against the write.
class SharedState {
public:
    static SharedState& instance();

    long long incr(const std::string& key, long long by);
    Value     get(const std::string& key) const;
    void      set(const std::string& key, Value v);
    bool      remove(const std::string& key);

private:
    mutable std::mutex          mutex_;
    std::map<std::string, Value> data_;
};

// Session state during a request.
struct SessionState {
    Value::Dict data;
    bool        loaded = false;   // the cookie has already been read once
    bool        dirty  = false;   // the handler modified it: it must be rewritten
    std::string secret;           // empty = sessions not configured
};

struct Template;

struct NativeCtx {
    lumen::Request&  req;
    lumen::Response& res;

    // Compiled templates of the module.  They are compiled at startup, so
    // rendering is walking them, not parsing them.
    const std::vector<Template>* templates = nullptr;

    // User function table of the module.  The expressions of a template need it
    // to be able to call a function from the .lum.
    const FunctionTable* functions = nullptr;

    // Only on sse routes: the stream writer, created by the driver before
    // starting the VM.  Null on the rest, and the sse builtins check it.
    lumen::SSEWriter* sse = nullptr;

    // Transaction in progress per module: name -> pinned worker.  While it
    // exists, every query of that module goes through the same connection.
    std::map<std::string, int> pinned_workers;

    // Worker that ran the last exec of each module.  `last_id()` has to go
    // through that SAME connection: the generated identifier does not exist on
    // the others.
    std::map<std::string, int> last_exec_workers;

    // Multipart parts already parsed.  A File in the language keeps the index
    // of its part here, not the bytes: copying a File is copying an int.
    const std::vector<lumen::MultipartPart>* parts   = nullptr;
    bool                                      uploads = false;

    // Only on ws routes: the connection already established.
    lumen::WSConnection* ws = nullptr;

    // Session: signed cookie, no server-side state.  It is loaded lazily on
    // first access and only rewritten if the handler modifies it.
    SessionState* session = nullptr;

    // Only in an `on error` handler: the code, the reason, and —when the error
    // comes from validating a body— the complete list of messages.
    int                             error_code    = 0;
    std::string                     error_message;
    const std::vector<std::string>* error_messages = nullptr;

    // Claims of the Authorization: Bearer JWT, already verified by the driver.
    const Value* jwt_claims = nullptr;
    bool         jwt_ok     = false;

    // Set by any builtin that writes the response (text, render, redirect...).
    // If it is still false when the handler ends, the returned value is
    // serialized as JSON.
    bool response_written = false;
};

// A builtin returns a Value and, if it fails, writes the reason into `error`.
using NativeFn = Value (*)(NativeCtx& ctx, std::vector<Value>& args,
                           std::string& error);

struct NativeDef {
    const char* name;
    int         min_args;
    int         max_args;    // -1 = no limit
    NativeFn    fn;          // null if it is asynchronous
    bool        is_async = false;
};

// Builtins that suspend.  The VM does not run them: it hands them back to the
// driver, which is the one that can do a real co_await on the engine.  The
// identifier is the table index itself, so it is looked up by name only once.

// Stable index of the builtin in the table, or -1 if it does not exist.  The
// emitter stores it in the instruction, so the VM never looks it up at runtime.
int              native_id(const std::string& name);

// Resolves `object.member` of a reserved object (sse.send, sse.open...) to the
// builtin implementing it, or -1 if that combination does not exist.  The
// mapping lives next to the table so adding a member is touching one place.
int              member_native_id(const std::string& object, const std::string& member);

// Method on a value: "hi".starts_with(...), list.add(...), file.save(...).
// Unlike the reserved objects, the receiver is known at runtime, so the
// dispatch is by type inside the VM.
Value call_method(NativeCtx& ctx, Value& receiver, const std::string& name,
                  std::vector<Value>& args, std::string& error);

// One of the methods call_method() recognizes, with the arguments it takes and
// what it returns.
struct BuiltinMethod {
    const char* name;
    int         min_args;
    int         max_args;
    // Type of the result, or nullptr if it returns the receiver itself —which
    // is what the chainable ones do: .status(), .add()...—.  Knowing it allows
    // checking to continue past the dot: s.upper().recortar() also fails at
    // compile time.
    const char* devuelve;
};

// The methods that exist for a Lumen Script type: "string", "int", "List", ...
//
// Returns nullptr when the type has no closed list —a user class, or a value
// whose type is only known at run time— and then there is nothing to check at
// compile time.
//
// The list lives glued to call_method() on purpose: they are two sides of the
// same thing, and adding a method has to be touching one place.
const std::vector<BuiltinMethod>* methods_of(const std::string& type);
bool             is_reserved_object(const std::string& name);
bool             is_db_module(const std::string& name);

// Messages of the last failed validation on this thread.  They are filled when
// building the 422 and read by the `on error` handler of the same request;
// there is no suspension between the two moments, so requests cannot cross.
std::vector<std::string>& last_validation_messages();
const NativeDef& native_at(int id);
int              native_count();

} // namespace lumen_script
