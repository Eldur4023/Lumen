#pragma once
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <vector>
#include "token.hpp"

namespace lumen_script {

// ─── Types ───────────────────────────────────────────────────────────────────

struct TypeRef {
    std::string          name;      // int, string, List, User, ...
    std::vector<TypeRef> args;      // parametros de List<T> / Dict<K,V>
    bool                 optional = false;   // sufijo '?'
    SourceLoc            loc;

    std::string str() const;
};

// ─── Expressions ─────────────────────────────────────────────────────────────

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

enum class ExprKind {
    StringLit, IntLit, FloatLit, BoolLit, NullLit,
    Ident, This, Member, Index, Call,
    Unary, Binary, Ternary, Await,
    PreStep, PostStep,      // ++x / --x  y  x++ / x--
    ListLit, DictLit,
};

// A call argument: positional, or named (`error_window="..."`).
struct Arg {
    std::string name;    // empty if positional
    ExprPtr     value;
    SourceLoc   loc;
};

struct DictEntry {
    ExprPtr key;
    ExprPtr value;
};

struct Expr {
    ExprKind  kind;
    SourceLoc loc;

    // Literals
    std::string text;        // StringLit / Ident / Member.name / operador
    long long   int_value  = 0;
    double      float_value = 0;
    bool        bool_value = false;

    // Structure
    ExprPtr                object;    // Member/Index/Call: receptor
    ExprPtr                lhs, rhs;  // Binary / Ternary(cond=object)
    std::vector<Arg>       args;      // Call
    std::vector<ExprPtr>   items;     // ListLit
    std::vector<DictEntry> entries;   // DictLit
};

// ─── Statements ──────────────────────────────────────────────────────────────

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;
using Block   = std::vector<StmtPtr>;

enum class StmtKind {
    Return, ExprStmt, VarDecl, Assign,
    If, While, For, Require, Try, Break, Continue,
};

struct Stmt {
    StmtKind  kind;
    SourceLoc loc;

    ExprPtr value;      // Return / ExprStmt / VarDecl.init / Assign.rhs / cond
    ExprPtr target;     // Assign.lhs / Require.else / For.iterable

    TypeRef     type;   // VarDecl / For
    std::string name;   // VarDecl / For / Try.catch

    Block body;         // If-then / While / For / Try
    Block orelse;       // If-else / Try-catch

    // else-if chain: every link is a complete If inside `orelse`.
};

// ─── Declarations ────────────────────────────────────────────────────────────

// A parameter of an endpoint or a function.
struct Param {
    TypeRef     type;
    std::string name;
    ExprPtr     default_value;   // null if it has none
    SourceLoc   loc;
};

// A class field.
struct Field {
    TypeRef     type;
    std::string name;
    SourceLoc   loc;
};

// A rule of the `validate:` block: a boolean expression and the message
// emitted when it is false.
struct ValidateRule {
    ExprPtr     condition;
    std::string message;
    SourceLoc   loc;
};

struct FnDecl {
    TypeRef            return_type;
    std::string        name;
    std::vector<Param> params;
    Block              body;
    SourceLoc          loc;
};

// A constructor.  Without a body, it assigns each parameter to the field of the
// same name; with a body, it runs it with `this` already created and every
// field set to null.
struct CtorDecl {
    std::vector<Param> params;
    Block              body;
    bool               has_body = false;
    SourceLoc          loc;
};

struct ClassDecl {
    std::string               name;
    std::vector<Field>        fields;
    std::vector<ValidateRule> rules;
    std::vector<FnDecl>       methods;
    std::vector<CtorDecl>     ctors;
    SourceLoc                 loc;
};

// A `require X else Y` guard declared at group level.  It is prepended to the
// body of every route in the group, from the outside in.
// They are shared instead of copied: a guard declared once is used by every
// route in the group, and the emitter only reads them.
struct Guard {
    std::shared_ptr<Expr> condition;
    std::shared_ptr<Expr> otherwise;
    SourceLoc             loc;
};

struct RouteDecl {
    std::string              method;    // "GET", "POST", ..., "SSE", "WS"
    std::string              pattern;   // "/users/:id", already with the group prefix
    std::vector<Param>       params;
    std::vector<std::string> origins;   // ws only
    std::vector<Guard>       guards;    // accumulated from the enclosing groups
    Block                    body;
    SourceLoc                loc;
    SourceLoc                pattern_loc;
};

// `on error 404:` / `on error:` — what to serve when the engine produces an
// error code.  code == 0 is the global handler.
struct ErrorDecl {
    int       code = 0;
    Block     body;
    SourceLoc loc;
};

struct StaticMount {
    std::string url_prefix;
    std::string fs_root;
    bool        spa = false;
    SourceLoc   loc;
};

struct AppDecl {
    std::string              name;
    std::string              version;
    int                      port = 8080;
    std::string              templates_dir = "./templates";
    std::vector<StaticMount> statics;
    bool                     docs = false, health = false, metrics = false;

    // session: and jwt: — the secrets are resolved at compile time, usually
    // with env(), so they do not end up written in the .lum.
    std::string session_secret;
    int         session_max_age = 86400;
    bool        session_secure  = true;
    std::string jwt_secret;
    std::string jwt_issuer;

    // Module configuration blocks: name -> key -> value.
    std::map<std::string, std::map<std::string, std::string>> modules;

    SourceLoc                loc;
    bool                     present = false;
};

struct Program {
    // Imported modules.  `sqlite.query(...)` can only be used if there is an
    // `import sqlite`.
    std::set<std::string>  imports;
    std::vector<ClassDecl> classes;
    std::vector<FnDecl>    functions;
    std::vector<RouteDecl> routes;
    std::vector<ErrorDecl> errors;
    AppDecl                app;
};

} // namespace lumen_script
