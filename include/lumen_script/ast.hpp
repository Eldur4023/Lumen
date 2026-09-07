#pragma once
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <vector>
#include "token.hpp"

namespace lumen_script {

// ─── Tipos ───────────────────────────────────────────────────────────────────

struct TypeRef {
    std::string          name;      // int, string, List, User, ...
    std::vector<TypeRef> args;      // parametros de List<T> / Dict<K,V>
    bool                 optional = false;   // sufijo '?'
    SourceLoc            loc;

    std::string str() const;
};

// ─── Expresiones ─────────────────────────────────────────────────────────────

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

enum class ExprKind {
    StringLit, IntLit, FloatLit, BoolLit, NullLit,
    Ident, This, Member, Index, Call,
    Unary, Binary, Ternary, Await,
    PreStep, PostStep,      // ++x / --x  y  x++ / x--
    ListLit, DictLit,
};

// Un argumento de llamada: posicional, o con nombre (`error_window="..."`).
struct Arg {
    std::string name;    // vacio si es posicional
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

    // Literales
    std::string text;        // StringLit / Ident / Member.name / operador
    long long   int_value  = 0;
    double      float_value = 0;
    bool        bool_value = false;

    // Estructura
    ExprPtr                object;    // Member/Index/Call: receptor
    ExprPtr                lhs, rhs;  // Binary / Ternary(cond=object)
    std::vector<Arg>       args;      // Call
    std::vector<ExprPtr>   items;     // ListLit
    std::vector<DictEntry> entries;   // DictLit
};

// ─── Sentencias ──────────────────────────────────────────────────────────────

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

    // Cadena else-if: cada eslabon es un If completo dentro de `orelse`.
};

// ─── Declaraciones ───────────────────────────────────────────────────────────

// Un parametro de endpoint o de funcion.
struct Param {
    TypeRef     type;
    std::string name;
    ExprPtr     default_value;   // nulo si no tiene
    SourceLoc   loc;
};

// Un campo de clase.
struct Field {
    TypeRef     type;
    std::string name;
    SourceLoc   loc;
};

// Una regla del bloque `validate:`: expresion booleana y el mensaje que se
// emite cuando es falsa.
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

// Un constructor.  Sin cuerpo, asigna cada parametro al campo del mismo
// nombre; con cuerpo, lo ejecuta con `this` ya creado y con todos los campos
// a null.
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

// Una guarda `require X else Y` declarada a nivel de grupo.  Se antepone al
// cuerpo de cada ruta del grupo, en orden de fuera hacia dentro.
// Se comparten en vez de copiarse: una guarda declarada una vez la usan todas
// las rutas del grupo, y el emisor solo las lee.
struct Guard {
    std::shared_ptr<Expr> condition;
    std::shared_ptr<Expr> otherwise;
    SourceLoc             loc;
};

struct RouteDecl {
    std::string              method;    // "GET", "POST", ..., "SSE", "WS"
    std::string              pattern;   // "/users/:id", ya con el prefijo del grupo
    std::vector<Param>       params;
    std::vector<std::string> origins;   // solo ws
    std::vector<Guard>       guards;    // acumuladas de los grupos que la envuelven
    Block                    body;
    SourceLoc                loc;
    SourceLoc                pattern_loc;
};

// `on error 404:` / `on error:` — que servir cuando el motor produce un codigo
// de error.  code == 0 es el manejador global.
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

    // session: y jwt: — los secretos se resuelven al compilar, normalmente con
    // env(), para que no acaben escritos en el .lum.
    std::string session_secret;
    int         session_max_age = 86400;
    bool        session_secure  = true;
    std::string jwt_secret;
    std::string jwt_issuer;

    // Bloques de configuracion de modulos: nombre -> clave -> valor.
    std::map<std::string, std::map<std::string, std::string>> modules;

    SourceLoc                loc;
    bool                     present = false;
};

struct Program {
    // Modulos importados.  Solo se puede usar `sqlite.query(...)` si hay un
    // `import sqlite`.
    std::set<std::string>  imports;
    std::vector<ClassDecl> classes;
    std::vector<FnDecl>    functions;
    std::vector<RouteDecl> routes;
    std::vector<ErrorDecl> errors;
    AppDecl                app;
};

} // namespace lumen_script
