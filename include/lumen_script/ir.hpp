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

// Argumento ya resuelto: el checker ya aplico los valores por defecto
// (FnSig::defaults) y el orden posicional que le corresponde a cada nombrado
// admitido (solo render() los admite hoy, ver forma 6) -- IrArg nunca lleva
// un nombre pendiente de resolver.
struct IrArg {
    IrExprPtr value;
    SourceLoc loc;
};

struct IrDictEntry {
    IrExprPtr key;
    IrExprPtr value;
};

struct IrExpr {
    IrExprKind kind;
    SourceLoc  loc;

    // Tipo ya resuelto por el checker. Type::unknown() es un resultado
    // legitimo (lo mismo que hoy devuelve tipo_de() para lo que no se puede
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

    // Estructura general, mismo reuso que Expr segun el kind:
    //   Member/Index/Call  -> object es el receptor
    //   Unary              -> object es el operando
    //   Binary             -> lhs, rhs
    //   Ternary            -> object es la condicion, lhs/rhs son las ramas
    //   Await/PreStep/PostStep -> object es la subexpresion
    IrExprPtr object;
    IrExprPtr lhs, rhs;

    // Call
    IrCallShape        call_shape = IrCallShape::Invalid;
    std::vector<IrArg> args;
    // Segun call_shape: el indice ya resuelto en la tabla que corresponda
    // (FunctionSigs::index en UserFunctionCall/ClassMethodCall, el numero de
    // parametros ya usado para indexar ClassSig::ctors en ConstructorCall).
    // -1 si esa forma no resuelve por indice (BuiltinGlobalCall,
    // BuiltinMethodCall y ReservedMemberCall resuelven por nombre).
    int call_index = -1;
    // Nombre ya resuelto que necesita el opcode final: el modulo inyectado en
    // DbModuleCall, el nombre reservado (sse/ws/error) en ReservedMemberCall,
    // o el nombre del builtin/metodo en BuiltinGlobalCall/BuiltinMethodCall.
    // Vacio si call_shape no lo necesita.
    std::string call_name;
    bool        awaited = false;

    // ListLit / DictLit
    std::vector<IrExprPtr>   items;
    std::vector<IrDictEntry> entries;
};

} // namespace lumen_script
