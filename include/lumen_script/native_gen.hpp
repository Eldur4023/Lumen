#pragma once
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hpp"
#include "emitter.hpp" // ClassSigs
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

// Un campo de clase ya reducido a su Type -- solo entra a ClaseNativa::campos
// si es representable (ver tipo_elemento_contenedor_soportado en
// native_gen.cpp): int/float/bool/string, nunca opcional -- exactamente el
// mismo subconjunto que un elemento de List o un valor de Dict, porque el
// lenguaje ya restringe los campos de clase a escalares (project.cpp). El
// ORDEN de esta lista fija el layout del struct C++ generado.
//
// Fase 5.7 (COMPILACION-NATIVA.md): un campo `?` (opcional) SI tiene
// entrada aqui, a diferencia del resto de sitios donde `?` deja algo fuera
// -- necesario porque un parametro de cuerpo de peticion (la unica via
// real de construir una instancia con datos externos) puede traer
// cualquier subconjunto de sus campos opcionales ausente. `tipo` es el
// ALMACENAMIENTO real: Type::json() si `opcional` (un campo que puede ser
// null en tiempo de ejecucion es, por definicion, un valor dinamico -- se
// trata exactamente como el resultado de una consulta, ver
// es_json_dinamico()), o el tipo escalar declarado tal cual si no.
// `kind_escalar`/`ortografia` conservan el tipo ORIGINAL (antes de
// envolverlo en Json) para cuando el binding del cuerpo de la peticion
// necesite el nombre exacto ("se esperaba double", no "se esperaba Json").
struct CampoNativo {
    std::string nombre;
    Type        tipo = Type::unknown();
    bool        opcional      = false;
    Type::Kind  kind_escalar  = Type::Kind::Void; // Int/Float/Bool/String, SIEMPRE poblado
    std::string ortografia;                       // "int"/"long"/"float"/"double"/"bool"/"string"
};

// Lo que native_gen.cpp necesita saber de una clase para representarla de
// forma nativa: el layout (campos, en orden de declaracion) y la firma de
// cada metodo (para comprobar una llamada, igual que TablaFirmas para
// funciones sueltas -- pero por metodo, no globalmente, porque dos clases
// distintas pueden tener un metodo con el mismo nombre). Una clase con
// algun campo no representable (List/Dict -- una clase como campo de otra
// clase no existe en el lenguaje) sencillamente no tiene entrada aqui:
// cualquier uso suyo (This/Member/ConstructorCall/ClassMethodCall) se
// queda sin poder demostrar su tipo.
//
// Fase 5.8: `reglas` es cada `validate:` ya compilada a IR y reprobada
// como Type::Kind::Bool contra los CAMPOS de la clase (0..N-1, mismo
// orden que `campos` -- ver construir_clases()) -- generar_ruta_nativa()
// las evalua, en orden, justo antes de construir la instancia, igual que
// bind_body() (project.cpp). `reglas_ok`: si la clase declara ALGUN
// `validate:` y CUALQUIER regla no demuestra Bool, esta en false y
// `reglas` queda VACIO a proposito -- la clase entera se rechaza como
// parametro de cuerpo (generar_ruta_nativa) en vez de compilar solo una
// parte: ejecutar unas reglas si y otras no dejaria pasar datos que
// deberian haber fallado una validacion que --native se salto en
// silencio, la misma clase de divergencia que este documento existe para
// evitar en todo lo demas.
struct ReglaNativa {
    std::string condicion_cpp; // ya generado (Generador::expr), listo para pegar en un `if`
    std::string mensaje;
};
struct ClaseNativa {
    std::vector<CampoNativo>                     campos;
    std::unordered_map<std::string, FirmaNativa> metodos;
    std::vector<ReglaNativa>                      reglas;
    bool                                          reglas_ok = true;
};
// Por nombre de clase Lumen.
using TablaClases = std::unordered_map<std::string, ClaseNativa>;

// A que clase (y que papel) pertenece una funcion de la tabla global,
// indexada igual que FunctionTable/nombre_por_indice: `metodo` vacio
// significa que es un constructor. `tiene_cuerpo` solo importa para un
// constructor -- esta fase solo compila el que NO tiene cuerpo (el
// automapeo "un parametro por campo, en orden", ver COMPILACION-NATIVA.md):
// comprobar que un constructor CON cuerpo deja todos los campos con su
// tipo declarado (que ninguno se quede en `null`, el valor con el que
// arranca `MakeDict` en emit_ctor) exige un analisis de asignacion
// definida sobre las ramas del cuerpo que esta fase todavia no hace.
struct RolFuncion {
    std::string clase;
    std::string metodo;
    bool        tiene_cuerpo = false;
};
using TablaRoles = std::unordered_map<int, RolFuncion>;

// Construye TablaClases/TablaRoles a partir del programa entero y las
// firmas de clase ya resueltas (ClassSigs, de project.cpp) -- incluido el
// constructor implicito que build_class_signatures() sintetiza cuando una
// clase no declara ninguno: ese constructor NO vive en el AST
// (Program::classes[i].ctors), asi que esta funcion repite, a proposito,
// la misma regla de sintesis ("un parametro por campo, en orden") que ya
// aplica project.cpp. `fns`/`imports`: solo para poder construir un
// Emitter y obtener el IR de cada `validate:` (Emitter::check_condition)
// -- el mismo camino que ya usa build_classes() (project.cpp) para su
// canario de sombra, aqui capturado de verdad en vez de descartado.
void construir_clases(const Program& prog, const ClassSigs& clases_sig,
                      const FunctionSigs& fns, const std::set<std::string>* imports,
                      TablaClases& clases, TablaRoles& roles);

// `nombre_por_indice[i]` es el nombre Lumen de la funcion de indice `i` en
// la tabla del modulo (FnSig::index) -- hace falta para traducir una
// llamada (IrExpr::Call, call_shape == UserFunctionCall) de vuelta a un
// nombre, porque el IR solo lleva el indice ya resuelto. `firmas` es la
// tabla de arriba, para comprobar el tipo de cada argumento contra el
// parametro correspondiente del destino. `clases`/`roles` son las tablas de
// arriba, para cuando el cuerpo de esta funcion usa una clase (crea una
// instancia, llama a un metodo, lee/escribe un campo).
std::optional<FuncionNativa> generar_funcion_nativa(const FnDecl& fn, const IrBlock& body,
                                                     const std::vector<std::string>& nombre_por_indice,
                                                     const TablaFirmas& firmas,
                                                     const TablaClases& clases,
                                                     const TablaRoles& roles);

// Igual que generar_funcion_nativa(), para el cuerpo de un metodo: `this`
// ocupa la ranura 0 (antes que los parametros, ver Emitter::check_method),
// al reves que en un constructor. Nunca tiene wrapper de ABI -- un
// receptor de tipo clase nunca cruza la ABI fija (tipo_abi_soportado), asi
// que ni falta que hace intentarlo -- y el simbolo C++ se nombra
// `l_<clase>_<metodo>`, no solo `l_<metodo>`, porque dos clases distintas
// pueden compartir el nombre de un metodo (Lumen no tiene sobrecarga, pero
// cada clase es su propio espacio de nombres).
std::optional<FuncionNativa> generar_metodo_nativo(const std::string& clase, const FnDecl& fn,
                                                    const IrBlock& body,
                                                    const std::vector<std::string>& nombre_por_indice,
                                                    const TablaFirmas& firmas,
                                                    const TablaClases& clases,
                                                    const TablaRoles& roles);

// El texto C++ del struct/caja de una clase nativa -- LPunto, con la misma
// semantica de referencia real (§8) que LList/LDict (caja con refcount no
// atomico), pero SIN plantilla: cada clase Lumen tiene su propio conjunto
// FIJO de campos tipados, no un elemento homogeneo. El unico constructor
// (aparte de copia/movimiento) toma un valor por campo, en el ORDEN de
// `clase.campos` -- es, a la vez, el automapeo del unico constructor que
// esta fase compila (ver RolFuncion::tiene_cuerpo) y la unica forma de
// construir una instancia en C++ generado.
std::string generar_clase_runtime(const std::string& nombre_clase, const ClaseNativa& clase);

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

// `Dict<string, V>` para V en int/float/bool/string: mismo diseño que
// `LList<T>` (caja con refcount no atomico, semantica de referencia real),
// una plantilla `LDict<V>`. Sin lectura por indice a proposito -- una clave
// ausente da `null` en el VM (vm.cpp::GetIndex), un tipo distinto del valor
// declarado que esta fase no puede representar (misma ambiguedad que
// `a / b` entre dos int) -- solo escritura y los metodos `has`/`keys`.
std::string dict_runtime_prelude();

// ── Fase 4: rutas HTTP sincronas ────────────────────────────────────────────
//
// Una ruta NO cruza la ABI fija de native_abi.hpp -- no la necesita, porque
// nadie la llama desde bytecode: la unica que la invoca es el propio
// build_routes() (project.cpp), en C++ normal, asi que el simbolo `extern
// "C"` que exporta puede tener la firma real que haga falta
// (lumen::Request&, lumen::Response&) en vez del NativeValue* generico de
// una funcion. Eso, a su vez, es lo que permite que el valor de retorno sea
// heterogeneo (un dict JSON con int/string/float mezclados, el caso comun):
// se serializa con lumen_script::Value, no con Dict<V> (homogeneo, ver
// dict_runtime_prelude), justo el "pendiente" que documenta
// COMPILACION-NATIVA.md.
//
// Alcance de este primer corte (ver generar_ruta_nativa): solo parametros
// escalares de patron (:id) o query string, SIN valor por defecto -- un
// File, un parametro de tipo clase (cuerpo de peticion) o cualquier `?`
// dejan la ruta entera fuera, igual que un cuerpo que use `session`/`jwt`/
// `render`/`await` (Comprobador::block_compilable ya los rechaza: no forman
// parte de ningun IrExprKind/IrStmtKind que reconozca). Cuando la ruta
// entera es representable, el handler nativo sustituye enteramente a
// bind_params/prepare_args/begin_auth/el VM -- no los llama, hace su propio
// binding (identico a prepare_args, mismo formato de 400) porque son ellos
// mismos codigo generado.
struct RutaNativa {
    std::string simbolo;    // "lumen_native_route_<indice>"
    std::string cuerpo_cpp; // "extern \"C\" void/Task<void> <simbolo>(lumen::Request&, lumen::Response&) { ... }"

    // Fase 5: un cuerpo que usa `await` (hoy: solo `await sleep(ms)`, ver
    // Comprobador::usa_await()) se genera como `lumen::Task<void>` de
    // verdad -- una corrutina, no una funcion plana -- porque es el UNICO
    // punto de entrada nativo que build_routes() invoca directamente (nadie
    // mas necesita suspenderse a mitad). `simbolo` es el mismo en los dos
    // casos; compilar_nativo() usa este campo para saber en cual de las dos
    // tablas de NativeModule (rutas_por_indice / rutas_async_por_indice)
    // dejar el puntero que resuelva dlsym().
    bool asincrona = false;
};

// Genera el C++ de una ruta, o nullopt si algo de ella (parametros o cuerpo)
// cae fuera de lo que esta fase representa -- ver el comentario de
// RutaNativa. `indice` es la posicion de `route` en Program::routes: hace
// falta para nombrar el simbolo (dos rutas nunca comparten indice) y para
// que compilar_nativo() sepa en que hueco de NativeModule::rutas_por_indice
// dejar el puntero ya resuelto por dlsym().
std::optional<RutaNativa> generar_ruta_nativa(const RouteDecl& route, const IrBlock& body,
                                              int indice,
                                              const std::vector<std::string>& nombre_por_indice,
                                              const TablaFirmas& firmas,
                                              const TablaClases& clases,
                                              const TablaRoles& roles);

// Funciones libres que necesita el binding de parametros que genera
// generar_ruta_nativa() -- mismo criterio, mismo formato de error, que
// coerce() en project.cpp, reproducido aqui porque el .cpp generado no
// puede llamar a una funcion `static`/de anonimo de ese otro archivo.
// Antepuesto una sola vez, igual que las demas *_runtime_prelude(), y SOLO
// cuando el modulo tiene al menos una ruta nativa (a diferencia de las
// otras: una funcion nativa nunca necesita lumen::Request/Response).
std::string route_runtime_prelude();

} // namespace lumen_script
