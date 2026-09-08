#pragma once
#include <optional>
#include <string>
#include <vector>

#include "ast.hpp"
#include "ir.hpp"

namespace lumen_script {

// Fase 2 de --native (COMPILACION-NATIVA.md): genera C++ real a partir del
// IR de una funcion de usuario -- "pura" en el sentido estrecho de esta
// fase, la primera que toca el documento a proposito ("esta fase es la que
// valida o tumba la tesis entera"): solo int/float/bool/void, solo control
// de flujo con if/while/return/break/continue/asignacion/declaracion de
// variable, y solo llamadas a otras funciones de usuario igual de puras.
// Nada de clases, contenedores (`for` incluido: itera una List), cadenas,
// `await` ni rutas -- eso es superficie de las fases 3 en adelante.
//
// No traduce: compila. El IR ya trae los tipos resueltos (Type de cada
// IrExpr) y las ranuras resueltas (slot de cada Ident) -- aqui no se
// vuelve a mirar nada de eso, solo se elige la representacion de maquina
// (int64_t/double/bool) y se emite la expresion C++ equivalente. Ver §7 de
// COMPILACION-NATIVA.md para la tabla de representacion completa (de la
// que esta fase solo cubre las tres primeras filas).
//
// No hay generacion parcial: si la funcion (o cualquier subexpresion/
// sentencia dentro) usa algo fuera de esta lista, no se genera nada de
// ella -- devuelve nullopt, y quien llama decide que hacer (hoy: no
// ofrecer --native para esa funcion, dejarla en bytecode).

struct FuncionNativa {
    std::string nombre_lumen;    // el nombre tal como aparece en el .lum
    std::string firma_cpp;       // "int64_t l_fib(int64_t l_n)"
    std::string cuerpo_cpp;      // "{ ... }", con llaves, indentado
};

// `nombre_por_indice[i]` es el nombre Lumen de la funcion de indice `i` en
// la tabla del modulo (FnSig::index) -- hace falta para traducir una
// llamada (IrExpr::Call, call_shape == UserFunctionCall) de vuelta a un
// nombre, porque el IR solo lleva el indice ya resuelto.
std::optional<FuncionNativa> generar_funcion_nativa(const FnDecl& fn, const IrBlock& body,
                                                     const std::vector<std::string>& nombre_por_indice);

} // namespace lumen_script
