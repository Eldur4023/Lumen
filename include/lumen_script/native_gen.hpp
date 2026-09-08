#pragma once
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hpp"
#include "ir.hpp"
#include "type.hpp"

namespace lumen_script {

// Fase 2 de --native (COMPILACION-NATIVA.md): genera C++ real a partir del
// IR de una funcion de usuario -- "pura" en el sentido estrecho de esta
// fase, la primera que toca el documento a proposito ("esta fase es la que
// valida o tumba la tesis entera"): int/float/bool/string/void, control de
// flujo con if/while/return/break/continue/asignacion/declaracion de
// variable, los 6 metodos de string, y llamadas a otras funciones de
// usuario igual de puras. Nada de clases, contenedores (`for` incluido:
// itera una List), `await` ni rutas -- eso es superficie de las fases 3 en
// adelante.
//
// No traduce: compila. El IR ya trae las ranuras resueltas (slot de cada
// Ident) -- eso no se vuelve a mirar. Los TIPOS, en cambio, si se
// recalculan aqui con un analisis propio (ver tipo_provable() en
// native_gen.cpp) en vez de confiar en el tipo declarado: Lumen Script no
// comprueba en ningun sitio que una variable, un retorno o un argumento
// mantengan el tipo con el que se declararon (una reasignacion `x =
// "otro tipo"` sobre un `int x` compila y corre sin aviso, con semantica
// dinamica de verdad) ni que `a / b` entre dos int de siempre Int (depende
// de si la division es exacta, en tiempo de ejecucion) -- confiar
// ciegamente en el tipo declarado, que es lo que hacia esta fase al
// principio, generaba C++ que compilaba pero daba resultados distintos a
// la VM en silencio. tipo_provable() es la correccion: solo acepta una
// expresion como compilable cuando puede DEMOSTRAR, con las mismas reglas
// dinamicas que aplica el VM, que su tipo nunca puede ser otro.
//
// No hay generacion parcial: si la funcion (o cualquier subexpresion/
// sentencia dentro) usa algo fuera de esta lista, o algo cuyo tipo no se
// puede demostrar, no se genera nada de ella -- devuelve nullopt, y quien
// llama decide que hacer (hoy: no ofrecer --native para esa funcion,
// dejarla en bytecode).

struct FuncionNativa {
    std::string nombre_lumen;    // el nombre tal como aparece en el .lum
    std::string firma_cpp;       // "int64_t l_fib(int64_t l_n)"
    std::string cuerpo_cpp;      // "{ ... }", con llaves, indentado

    // La funcion de arriba tiene la firma C++ que le corresponde por sus
    // tipos reales -- util para ensamblar un .cpp legible, pero inutil para
    // cargarla con dlsym() sin conocer esa firma de antemano. `simbolo_abi`
    // es el nombre exportado `extern "C"` de un wrapper con la firma fija
    // CompiledFn (ver native_abi.hpp), y `wrapper_cpp` es su definicion
    // completa: descomprime cada NativeValue al tipo real del parametro,
    // llama a la funcion de arriba, y empaqueta el resultado de vuelta.
    // Ambos quedan vacios si la frontera de la funcion no cruza la ABI fija
    // (hoy: usa `string`) -- el cuerpo de arriba se genera igual, solo se
    // queda sin wrapper (ver tipo_abi_soportado en native_gen.cpp).
    std::string simbolo_abi;     // "lumen_native_fib"
    std::string wrapper_cpp;     // definicion extern "C" completa del wrapper
};

// La firma de una funcion Lumen, en los terminos que necesita
// tipo_provable() para comprobar una llamada: el tipo completo (no solo el
// Kind) de cada parametro y del retorno -- List<int> y List<string> tienen
// el mismo Kind pero son tipos distintos, y Type::operator== ya sabe
// comparar eso.
struct FirmaNativa {
    std::vector<Type> params;
    Type               retorno = Type::void_();
};
// Por nombre de funcion Lumen -- construida una sola vez por
// compilar_nativo() a partir de TODO el programa (no solo las funciones que
// terminan compilando), porque una funcion nativa puede llamar a otra que
// el mapa (alfabetico) todavia no proceso.
using TablaFirmas = std::unordered_map<std::string, FirmaNativa>;

// `nombre_por_indice[i]` es el nombre Lumen de la funcion de indice `i` en
// la tabla del modulo (FnSig::index) -- hace falta para traducir una
// llamada (IrExpr::Call, call_shape == UserFunctionCall) de vuelta a un
// nombre, porque el IR solo lleva el indice ya resuelto. `firmas` es la
// tabla de arriba, para comprobar el tipo de cada argumento contra el
// parametro correspondiente del destino.
std::optional<FuncionNativa> generar_funcion_nativa(const FnDecl& fn, const IrBlock& body,
                                                     const std::vector<std::string>& nombre_por_indice,
                                                     const TablaFirmas& firmas);

// El texto C++ de la definicion de NativeValue, identico al de
// native_abi.hpp: hace falta duplicarlo dentro del .cpp que se compila a
// biblioteca compartida porque dlopen no comparte cabeceras del proyecto con
// la biblioteca cargada, solo un ABI compatible en tiempo de enlazado -- y
// dos definiciones textualmente identicas de un tipo POD lo son. Quien
// ensambla el fichero final (native_build.cpp) antepone esto una sola vez.
std::string abi_prelude();

// Funciones libres (`lumen_str_starts_with`, `lumen_str_upper`...) con la
// misma semantica exacta que la rama `string` de call_method() en
// natives.cpp -- lo que usa Generador::expr() al generar una llamada a uno
// de los metodos de string que reconoce metodo_string_soportado(). Se
// antepone una sola vez, igual que abi_prelude(); no cuesta nada incluirla
// aunque una funcion en concreto no use ningun metodo de string.
std::string string_runtime_prelude();

// El canal de error de una funcion nativa: `lumen_native_fail(msg)` deja
// `msg` en un buffer por hilo y lanza una excepcion vacia que solo el
// wrapper de cada funcion atrapa (ver generar_funcion_nativa), convirtiendo
// el fallo en NativeValue::Tag::Error -- una llamada nativa anidada
// (funcion nativa llamando a otra directamente en C++, sin wrapper de por
// medio) deja que la excepcion se propague sola por la pila de C++ hasta el
// wrapper mas externo, exactamente como una funcion Lumen sin `try` deja
// que el error suba hasta quien la llamo. `lumen_native_error_message` es
// el simbolo fijo (ver native_abi.hpp::ErrorMessageFn) que expone el
// mensaje del ultimo fallo en el hilo que llama. Usado para
// division/modulo por cero y, ahora, "indice fuera de rango" en List.
std::string error_runtime_prelude();

// `List<T>` para T en int/float/bool/string (§7): una plantilla `LList<T>`
// con una caja de refcount NO atomico (una funcion nativa nunca comparte
// una lista entre hilos: no cruza la ABI todavia, igual que `string` --
// ver tipo_abi_soportado) y semantica de referencia real (§8): copiar un
// `LList` copia el puntero a la caja, no los datos, asi que dos variables
// que apuntan a la misma lista ven las mutaciones la una de la otra, igual
// que el `Value::List` del VM. `lumen_get`/`lumen_set` comprueban el
// indice y usan lumen_native_fail() en vez de comportamiento indefinido.
std::string list_runtime_prelude();

} // namespace lumen_script
