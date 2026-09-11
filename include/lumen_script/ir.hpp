#pragma once
#include <memory>
#include <string>
#include <vector>

#include "ast.hpp"
#include "type.hpp"

namespace lumen_script {

// IR tipado de expresiones (COMPILACION-NATIVA.md fase 1, §1.1-1.2).
//
// TODAVIA NO ESTA CONECTADO a Emitter: este fichero es puramente aditivo, el
// mismo tipo de paso seguro que fue type.hpp antes de conectarse. Nada en
// Emitter construye ni consume IrExpr todavia.
//
// El objetivo de este IR es que `check_expr` (por escribir) y `emit_expr` (ya
// existente) dejen de tener que estar de acuerdo por las buenas sobre nombres
// resueltos: el checker recorre el Expr del AST UNA vez, hace exactamente las
// mismas comprobaciones y llamadas a declare_local/resolve_local que hace hoy
// emit_expr, y el resultado es un IrExpr que ya lleva el tipo y la ranura
// resueltos. Un emisor que solo consuma IrExpr no vuelve a resolver un nombre
// ni a llamar a error() -- eso es justo lo que evita la duplicacion de
// diagnosticos descrita en la fase 1.
//
// La forma de este struct calca a proposito la de Expr (ast.hpp): mismos
// campos de literal, mismo uso de `object`/`lhs`/`rhs` segun el kind (ver
// emit_expr para el porque de cada reuso: Ternary usa object como condicion y
// lhs/rhs como las dos ramas; Index usa object como receptor y lhs como el
// indice). Calcar la forma es intencional -- construir el IrExpr a partir del
// Expr es una traduccion nodo a nodo, no un rediseno.

struct IrExpr;
using IrExprPtr = std::unique_ptr<IrExpr>;

enum class IrExprKind {
    StringLit, IntLit, FloatLit, BoolLit, NullLit,
    Ident, This, Member, Index, Call,
    Unary, Binary, Ternary, Await,
    PreStep, PostStep,
    ListLit, DictLit,
};

// Las 8 formas de llamada que distingue emit_call hoy (COMPILACION-NATIVA.md
// §1.2). No es "una llamada generica con argumentos": cada forma tiene su
// propia regla de aridad/nombrados, su propia necesidad de await, y su propio
// opcode/backend de destino, asi que el checker tiene que decidir CUAL es
// antes de que el emisor (de bytecode o nativo) pueda actuar.
enum class IrCallShape {
    DbModuleCall,        // 1. sqlite.query(...)                    -> CallAsync
    ReservedMemberCall,  // 2. sse.send(...) / ws.send(...) / error.foo(...)
    UserFunctionCall,    // 3. fn de usuario, resuelta contra FunctionSigs
    ConstructorCall,     // 4. Clase(...), resuelta por aridad contra ctors
    ClassMethodCall,     // 5. metodo con receptor de tipo estatico conocido
    BuiltinGlobalCall,   // 6. len(...)/sleep(...)/render(...)
    BuiltinMethodCall,   // 7. s.upper()/xs.add(v)... estatico o dinamico
    Invalid,             // 8. ninguna de las anteriores: error de compilacion
};

// Argumento ya resuelto: el checker ya comprobo que los nombrados son
// validos donde aparecen (solo render(), forma 6, los admite hoy) y ya
// relleno los que faltan con su valor por defecto (FnSig::defaults) en las
// formas que los tienen. `name` viaja vacio para un argumento posicional o
// para un valor por defecto rellenado por el checker; solo lleva contenido
// en un nombrado de verdad (`render(x, k=v)`), que es la unica informacion
// de un IrArg que no esta ya en su `value` -- hace falta para que quien
// consuma el IR pueda reconstruir el Dict de variables (ver emit_call).
struct IrArg {
    std::string name;
    IrExprPtr   value;
    SourceLoc   loc;
};

struct IrDictEntry {
    IrExprPtr key;
    IrExprPtr value;
};

struct IrExpr {
    IrExprKind kind;
    SourceLoc  loc;

    // Tipo ya resuelto por el checker. Type::unknown() es un resultado
    // legitimo (lo mismo que hoy devuelve type_of() para lo que no se puede
    // saber en compilacion), no una marca de "todavia sin rellenar".
    Type type = Type::unknown();

    // Literales -- mismos campos que Expr, mismo significado.
    std::string text;
    long long   int_value   = 0;
    double      float_value = 0;
    bool        bool_value  = false;

    // Ident: la ranura ya resuelta por resolve_local/declare_local. Un Ident
    // que el checker no pudo resolver ya disparo error() y no llega a
    // construir un IrExpr -- por eso no hace falta aqui un estado de "no
    // resuelto", solo el indice real.
    int slot = -1;

    // Estructura general. Calca el reuso real de Expr (ast.hpp), verificado
    // contra emit_expr caso a caso -- no es simetrico a proposito, porque
    // Expr tampoco lo es:
    //   Member/Index/Call -> object es el receptor
    //   Binary            -> lhs, rhs son los operandos
    //   Ternary           -> object es la condicion, lhs/rhs son las ramas
    //   Unary             -> lhs es el operando
    //   Await             -> lhs es la llamada (siempre kind == Call)
    //   PreStep/PostStep  -> lhs es el objetivo, reconstruido como un IrExpr
    //                        Ident/Member/Index sintetico (ver check_expr):
    //                        Member/Index ahi NO comprueban el campo/objeto
    //                        con el mismo rigor que un Member/Index suelto,
    //                        porque emit_expr tampoco lo hace hoy -- ver el
    //                        comentario de check_expr sobre ese hueco.
    IrExprPtr object;
    IrExprPtr lhs, rhs;

    // Call
    IrCallShape        call_shape = IrCallShape::Invalid;
    std::vector<IrArg> args;
    // Segun call_shape: el indice ya resuelto en la tabla que corresponda
    // (FunctionSigs::index en UserFunctionCall/ClassMethodCall, el numero de
    // parametros ya usado para indexar ClassSig::ctors en ConstructorCall).
    // -1 si esa forma no resuelve por indice (BuiltinMethodCall resuelve por
    // nombre; ReservedMemberCall y BuiltinGlobalCall SI llevan call_index --
    // las dos terminan resolviendo al mismo native_id de hoy, solo cambia
    // call_shape segun de donde vino la llamada).
    int call_index = -1;
    // Nombre ya resuelto que necesita el opcode final: el modulo inyectado en
    // DbModuleCall, el nombre del metodo builtin en BuiltinMethodCall, o el
    // nombre completo ya formado ("obj.miembro" / el builtin) en
    // ReservedMemberCall/BuiltinGlobalCall. Vacio si call_shape no lo
    // necesita (UserFunctionCall/ClassMethodCall/ConstructorCall resuelven
    // por call_index, no por nombre).
    std::string call_name;
    bool        awaited = false;

    // Kind == Member representando un miembro de 0 argumentos de un objeto
    // reservado (`sse.open`, sin parentesis: emit_expr lo resuelve con
    // CallNative igual que una llamada, ver check_expr) reusa call_name (el
    // objeto reservado, ej. "sse") y call_index (el native_id ya resuelto)
    // en vez de anadir campos nuevos solo para este caso -- `text` sigue
    // llevando el nombre del miembro, como en cualquier otro Member.

    // ListLit / DictLit
    std::vector<IrExprPtr>   items;
    std::vector<IrDictEntry> entries;
};

// IR de sentencias (COMPILACION-NATIVA.md fase 1, paso 3 de §1.1: check_stmt
// ya existe y esta verificado -- ver Emitter::check_stmt -- este es el IR que
// deberia producir en vez de escribir a un DiagnosticBag). Aditivo, sin
// conectar: como con IrExpr, la forma calca a Stmt (ast.hpp) a proposito, y
// construirlo a partir de un Stmt real es traduccion nodo a nodo.

struct IrStmt;
using IrStmtPtr = std::unique_ptr<IrStmt>;
using IrBlock   = std::vector<IrStmtPtr>;

enum class IrStmtKind {
    Return, ExprStmt, VarDecl, Assign,
    If, While, For, Require, Try, Break, Continue,
};

// Las 4 formas de destino que distingue emit_stmt/check_stmt hoy en el caso
// StmtKind::Assign (mirando Stmt::target->kind): cada una es una operacion de
// runtime distinta, igual de bien diferenciada que las 8 formas de IrCall.
enum class IrAssignTarget {
    Session,  // session.x = v -> __session_set("x", v)
    Index,    // xs[i] = v     -> SetIndex
    Member,   // o.f = v       -> SetMember, tras comprobar que el campo existe
    Local,    // x = v         -> StoreLocal en la ranura ya resuelta
};

struct IrStmt {
    IrStmtKind kind;
    SourceLoc  loc;

    // Return / ExprStmt / VarDecl.init: la expresion (nulo en un Return sin
    // valor). If / While / Require: la condicion. Assign: el valor a asignar,
    // en las 4 formas.
    IrExprPtr value;

    // VarDecl / For: el tipo declarado y el nombre de la variable, y la
    // ranura que le asigno declare_local -- el consumidor de este IR no
    // vuelve a llamar a declare_local, solo StoreLocal en `slot`. Try: `name`
    // es el nombre del `catch` (vacio si no captura nada) y `slot` su ranura.
    Type        decl_type = Type::unknown();
    std::string name;
    int         slot = -1;

    // For: el iterable (`Stmt::target` original). Require: la expresion del
    // `else` (lo que se devuelve si la condicion es falsa).
    IrExprPtr target;

    // Assign: que forma de destino es, y las piezas que necesita cada una.
    // Nunca se rellena mas de lo que corresponde a `assign_target`.
    IrAssignTarget assign_target = IrAssignTarget::Local;
    IrExprPtr      assign_object;    // Index/Member: el receptor
    IrExprPtr      assign_index;     // Index: la expresion del indice
    std::string    assign_field;     // Session/Member: el nombre del campo
    int            assign_slot = -1; // Local: la ranura ya resuelta

    IrBlock body;    // If-then / While / For / Try
    IrBlock orelse;  // If-else / Try-catch
};

} // namespace lumen_script
