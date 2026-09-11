#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast.hpp"
#include "bytecode.hpp"
#include "diagnostic.hpp"
#include "ir.hpp"
#include "type.hpp"

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

    // ── Fase 1 (COMPILACION-NATIVA.md): checker en paralelo ─────────────────
    //
    // Reproduce TODAS las comprobaciones que hace emit_expr/emit_call (mismo
    // texto de error, mismo orden), pero sin tocar chunk_ ni locals_: no emite
    // bytecode y no declara ninguna ranura (las unicas declare_local() de hoy
    // son temporales de codegen -- PreStep/PostStep sobre un campo o un
    // indice -- que un paso de solo comprobacion no necesita).  Por eso puede
    // correr sobre el `this` REAL de una compilacion en curso, en cualquier
    // orden respecto a emit_expr/emit_call, sin corromper la numeracion de
    // ranuras: es un lector de locals_, nunca un escritor.
    //
    // Los errores van a `shadow`, NUNCA a diags_: todavia no es la fuente de
    // diagnosticos (eso llega cuando el corte real conecte esto a Emitter),
    // asi que un error de aqui no debe duplicar el que ya produce
    // emit_expr/emit_call por su cuenta.
    //
    // Ademas de comprobar, CONSTRUYE y devuelve el IrExpr correspondiente
    // (nulo si algo no compilo -- ya se llamo a shadow.error en el sitio
    // exacto). El tipo de cada nodo es tipo_de(e), sin excepcion: nunca un
    // tipo mas preciso inventado aqui, porque eso seria funcionalidad nueva
    // y no una reproduccion de lo que ya hace el compilador. Publico porque
    // la verificacion (comparar shadow contra diags_ real, y el shape del
    // IrExpr devuelto, sobre el corpus de tests/casos) vive en un binario de
    // pruebas aparte; nada en el compilador real llama a esto todavia.
    IrExprPtr check_expr(const Expr& e, DiagnosticBag& shadow) const;
    IrExprPtr check_call(const Expr& e, bool awaited, DiagnosticBag& shadow) const;

    // Contrapartida de check_expr para emit_condition: mismo reinicio de
    // locals_/route_method_/scope_depth_, mismas declaraciones de `names`,
    // pero llamando a check_expr en vez de a emit_expr.
    //
    // Lleva su propio `Chunk& out` -- igual que emit_condition -- aunque no
    // emita ni un opcode: declare_local() escribe chunk_->num_locals segun
    // avanza (la misma contabilidad que necesita el VM para dimensionar la
    // pila, la real, no una copia), asi que necesita un chunk_ valido desde
    // el primer momento y no puede depender de que alguien haya llamado antes
    // a emit_condition sobre el mismo Emitter para dejarlo puesto. Pasar el
    // MISMO chunk que ya se le paso a emit_condition (que es lo que hace hoy
    // el canario de project.cpp) es valido: declare_local() vuelve a anotar
    // los mismos nombres, pero num_locals ya no puede subir mas de lo que ya
    // subio, asi que no cambia nada observable.
    IrExprPtr check_condition(const Expr& e, const std::vector<NombreTipado>& names,
                              Chunk& out, DiagnosticBag& shadow);

    // check_stmt/check_block: la misma idea que check_expr, pero para
    // sentencias -- reproducen emit_stmt/emit_block rama a rama y construyen
    // el IrStmt/IrBlock correspondiente (nulo/vacio en caso de error, mismo
    // criterio que check_expr). A diferencia de check_expr, SI declaran
    // ranuras (VarDecl, el `for` desazucarado, el nombre de un `catch`):
    // esas SI son contabilidad de nombres real, no un temporal de codegen, y
    // hace falta que el checker la lleve para que el Ident de una sentencia
    // posterior resuelva bien. Por construccion no puede correr sobre el
    // `this` de una emision real en curso (pisaria sus ranuras) -- de ahi
    // check_route/check_function/etc. como puntos de entrada propios, cada
    // uno reiniciando el estado exactamente como su contrapartida emit_*,
    // para poder llamarse en secuencia sobre el mismo Emitter (primero la
    // via real, luego la sombra) sin interferir.
    IrStmtPtr check_stmt(const Stmt& s, DiagnosticBag& shadow);
    // Vacio si algun sentencia del bloque fallo (ya se reporto en su sitio);
    // igual que hoy con `failed_`, un bloque a medio construir no se usa.
    IrBlock   check_block(const Block& body, DiagnosticBag& shadow);

    // Emisor puro que consume el IrBlock que devuelve check_block/check_route/
    // etc.: no comprueba nada, confia en que el IR ya paso por el checker.
    // Publico (a diferencia de emit_stmt/emit_expr/emit_call sobre IrExpr/
    // IrStmt, que son privados e internos a este) porque tests/
    // emit_ir_shadow.cpp lo llama directamente para probar que el bytecode
    // que produce se COMPORTA igual que el del emisor viejo, ejecutado de
    // verdad en el VM -- es exactamente lo que hara emit_function una vez
    // conectado, asi que la prueba lo replica desde fuera.
    void emit_block(const IrBlock& body);

    // `out`: mismo motivo que en check_condition (declare_local necesita un
    // chunk_ valido). `out_body`, si no es nulo, recibe el IrBlock construido
    // (el mismo que ya se descarta hoy en project.cpp: check_route/etc. lo
    // usan solo para saber si `shadow` crecio).
    bool check_route(const RouteDecl& route, Chunk& out, DiagnosticBag& shadow,
                     IrBlock* out_body = nullptr);
    bool check_function(const FnDecl& fn, Chunk& out, DiagnosticBag& shadow,
                        IrBlock* out_body = nullptr);
    bool check_method(const std::string& cls, const FnDecl& m, Chunk& out,
                      DiagnosticBag& shadow, IrBlock* out_body = nullptr);
    bool check_ctor(const std::string& cls, const std::vector<std::string>& fields,
                    const CtorDecl& ct, Chunk& out, DiagnosticBag& shadow,
                    IrBlock* out_body = nullptr);
    bool check_error_handler(const ErrorDecl& decl, Chunk& out, DiagnosticBag& shadow,
                             IrBlock* out_body = nullptr);

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
    struct Local { std::string name; int depth; Type type; };
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
                       Type type = Type::unknown());
    const Type& local_type(const std::string& name) const;

    // Collects the operands of a '+' chain, on the raw AST — used by
    // check_call to look for request data glued directly into SQL, before
    // any IR exists yet.
    static void flatten_concat(const Expr& e, std::vector<const Expr*>& out);
    // True if `e` is, or concatenates in, a direct query()/header() call —
    // see the implementation in emitter.cpp for what this does and does not
    // catch.
    static bool looks_like_direct_request_data(const Expr& e);
    // Same idea as flatten_concat, but walking the IrExpr chain check_expr
    // already built — used by the bytecode emitter to batch a '+' chain
    // into one ConcatN instead of N-1 pairwise Adds.
    static void flatten_concat_ir(const IrExpr& e, std::vector<const IrExpr*>& out);

    int  resolve_local(const std::string& name) const;
    void begin_scope();
    void end_scope();

    // Static type of an expression, or Type::unknown() if it cannot be known.
    // Only looks at the obvious, with no inference: a literal, or a declared
    // variable. Stays in terms of Expr (not IrExpr) because the AST is what
    // check_expr/check_call/check_field look at when they need the static
    // type of a receiver — no IR version is needed.
    Type type_of(const Expr& e) const;

    // ── The real emitter: consumes the IrExpr/IrStmt that check_expr/
    // check_stmt already built and validated (with the real diags_ — see
    // emit_route/emit_function/etc. above). It checks nothing — not a call
    // to error(), not a name lookup (resolve_local/native_id/
    // is_reserved_object...): it trusts that the IR it receives already
    // passed the checker, reading already-resolved fields (slot, call_shape,
    // call_index, call_name, type) instead of resolving them. emit_block is
    // public (see the comment next to check_block) because
    // tests/emit_ir_shadow.cpp calls it directly to test execution
    // equivalence against the emitter that existed before the cut — what
    // emit_function does today.
    void emit_stmt(const IrStmt& s);
    void emit_expr(const IrExpr& e);
    void emit_call(const IrExpr& e);
    // True only if the IrExpr CAN be proven to be of type int — same rule as
    // is_int_expr(), but reading IrExpr::type (already resolved by
    // check_expr) instead of local_type(): no IrExpr needs to resolve a name
    // again.
    bool is_int_expr_ir(const IrExpr& e) const;
    // The builtins that arrive here with an already-resolved native_id
    // (ReservedMemberCall and BuiltinGlobalCall except render(), compiled
    // separately): the same shared tail emit_call has today.
    void emit_native_call(const IrExpr& e);
    // Method whose receiver has no known type at compile time (BuiltinMethodCall).
    void emit_method_call_dynamic(const IrExpr& e);
    void emit_compiled_render(const IrExpr& e);

    // Checks a builtin field/method against the closed list for the
    // receiver's type, reporting the error to `shadow` (the real diags_ from
    // the entry points, a separate DiagnosticBag in tests). Used by
    // check_expr/check_call.
    bool check_field(const Expr& object, const std::string& field, SourceLoc loc,
                     DiagnosticBag& shadow) const;
    bool check_builtin_method(const Expr& e, DiagnosticBag& shadow) const;

    // The desugaring shared by `require X else Y` (StmtKind::Require) and a
    // group guard (Guard): both are "if not X, return Y", same bytecode
    // (see emit_stmt/emit_route). Sharing this function avoids building the
    // IrStmt::Require twice with slightly different logic.
    IrStmtPtr check_require_like(SourceLoc loc, const Expr& cond, const Expr& otherwise,
                                 DiagnosticBag& shadow) const;
};

} // namespace lumen_script
