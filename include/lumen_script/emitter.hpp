#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast.hpp"
#include "bytecode.hpp"
#include "diagnostic.hpp"

namespace lumen_script {

// Translates the body of a route into bytecode.
//
// It resolves names to local slots at compile time, so the VM never looks a
// variable up by name: LoadLocal is a direct index.
// What the emitter needs to know about a user function in order to call it:
// where it is and what arguments it takes.
struct FnSig {
    size_t index    = 0;
    size_t required = 0;                    // parameters without a default value
    std::vector<const Expr*> defaults;      // one per parameter; null if it has none
};
using FunctionSigs = std::map<std::string, FnSig>;

// What the emitter needs to know about a class to build it and call its
// methods.  Both compile to functions with `this` as the first parameter.
struct ClassSig {
    std::map<std::string, FnSig> methods;
    std::map<size_t, size_t>     ctors;    // number of parameters -> index
    std::vector<std::string>     fields;
};
using ClassSigs = std::map<std::string, ClassSig>;

struct Template;

// A name visible inside a standalone expression, with its declared type.
//
// The type may be empty, and then nothing is checked about it: that is what
// happens with the variable of a {% for %}, whose type depends on what the
// list carries.
struct TypedName {
    std::string name;
    std::string type;

    // No type is the normal case, so only the name is written.  The literal
    // needs its own constructor: from const char* to TypedName there are two
    // conversions and the compiler only does one.
    TypedName(std::string n, std::string t = {})
        : name(std::move(n)), type(std::move(t)) {}
    TypedName(const char* n) : name(n) {}
};

// Where the templates are and where they are stored once compiled.
//
// When the emitter sees a render("x.html", k=v) with a literal name, it
// compiles the template RIGHT THERE against those keys.  That is why a typo
// inside a {{ }} shows up in `lumen --check` and not when someone loads the page.
struct TemplateCtx {
    std::string             dir;
    std::vector<Template>* table = nullptr;
};

class Emitter {
public:
    // `functions` maps a user function name to its index in the module table.
    // It is resolved while emitting, so the VM never looks one up by name.
    explicit Emitter(DiagnosticBag& diags, const FunctionSigs* functions = nullptr,
                     const ClassSigs* classes = nullptr,
                     const std::set<std::string>* imports = nullptr,
                     TemplateCtx* templates = nullptr)
        : diags_(diags), functions_(functions), classes_(classes), imports_(imports),
          templates_(templates) {}

    // The route parameters take the first slots, in order.
    // Returns false if something in the body cannot be compiled yet.
    bool emit_route(const RouteDecl& route, Chunk& out);

    // Body of a user function.
    bool emit_function(const FnDecl& fn, Chunk& out);

    // Method and constructor: they compile as functions with `this` as the
    // first parameter, so they reuse the VM frame stack with nothing special.
    bool emit_method(const std::string& cls, const FnDecl& m, Chunk& out);
    bool emit_ctor(const std::string& cls, const std::vector<std::string>& fields,
                   const CtorDecl& ct, Chunk& out);

    // Compiles a standalone expression with `names` already declared as locals,
    // in that order.  Used by the `validate` rules —each one becomes a tiny
    // chunk that takes the fields and returns a boolean— and by the expressions
    // inside a {{ }} in a template.
    //
    // The types travel with the names so that `name.mayusculas()` can be
    // rejected here: without them the emitter does not know `name` is a string
    // and the typo is discovered in production.
    bool emit_condition(const Expr& e, const std::vector<TypedName>& names,
                        Chunk& out);

    // Body of an `on error`: no parameters, with the `error` object available.
    bool emit_error_handler(const ErrorDecl& decl, Chunk& out);

private:
    DiagnosticBag&                       diags_;
    const FunctionSigs*                  functions_ = nullptr;
    const ClassSigs*                     classes_   = nullptr;
    const std::set<std::string>*         imports_   = nullptr;
    TemplateCtx*                        templates_ = nullptr;
    Chunk*         chunk_ = nullptr;
    bool           failed_ = false;

    // Method of the route being compiled: lets `sse.*` be rejected outside an
    // sse route at compile time, instead of leaving it to runtime.
    std::string route_method_;

    // The declared type is kept so `u.method()` can be resolved at compile
    // time: at runtime an instance is a Dict and would be indistinguishable.
    struct Local { std::string name; int depth; std::string type; };
    std::vector<Local> locals_;
    int                scope_depth_ = 0;

    // Pending jumps of the loop in progress.  Both are patched when it closes:
    // in a `for` the target of `continue` is the increment, which has not been
    // emitted yet when the `continue` appears inside the body.
    struct LoopCtx {
        std::vector<size_t> breaks;
        std::vector<size_t> continues;
    };
    std::vector<LoopCtx> loops_;

    void error(SourceLoc loc, std::string msg);

    int  declare_local(const std::string& name, SourceLoc loc,
                       const std::string& type = {});
    const std::string& local_type(const std::string& name) const;

    // True only if the expression CAN be proven to be of type int.  When in
    // doubt it says no: under-specializing just leaves generic code,
    // over-specializing would be a bug.
    bool is_int_expr(const Expr& e) const;
    static void flatten_concat(const Expr& e, std::vector<const Expr*>& out);
    int  resolve_local(const std::string& name) const;
    void begin_scope();
    void end_scope();

    void emit_block(const Block& body);
    void emit_stmt(const Stmt& s);
    void emit_expr(const Expr& e);
    void emit_compiled_render(const Expr& e);
    // Static type of an expression, or "" if it cannot be known.  It only looks
    // at the obvious, with no inference: a literal, or a declared variable.
    std::string type_of(const Expr& e) const;
    // Checks a method against the closed list for the receiver's type.
    // Returns false —having already reported the error— if that method does not exist.
    bool check_builtin_method(const Expr& e);
    // Same for a field, both reading and writing it.
    bool check_field(const Expr& object, const std::string& field, SourceLoc loc);
    void emit_call(const Expr& e, bool awaited);
    // Method whose receiver has no known type at compile time: it is pushed and
    // the VM does the dispatch by type.
    void emit_method_call_dynamic(const Expr& e);
};

} // namespace lumen_script
