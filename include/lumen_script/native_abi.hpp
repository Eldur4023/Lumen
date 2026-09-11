#pragma once
#include <cstdint>
#include <vector>

namespace lumen_script {

// El limite de la ABI entre el binario `lumen` y la biblioteca que produce
// --native (ver Fase 2 de COMPILACION-NATIVA.md, seccion 11): cualquier
// funcion compilada nativamente, sin importar su aridad o cual de los tres
// tipos primitivos use cada parametro, se exporta con esta misma firma fija.
// Así dlsym() y la VM no necesitan conocer la firma real de cada funcion —
// el propio C++ generado (ver native_gen.cpp) empaqueta y desempaqueta.
//
// Es un POD deliberadamente simple: se define por separado, con el mismo
// texto, en el .cpp que genera native_gen.cpp (ver
// native_gen.cpp::abi_prelude()) y aqui — dlopen no comparte cabeceras entre
// el binario y la biblioteca cargada, solo un ABI compatible en tiempo de
// enlazado, y dos definiciones textualmente identicas de un tipo POD lo son.
struct NativeValue {
    // Error: la funcion nativa detecto algo que en el VM es un error
    // controlado (division por cero, "% por cero"...), no un crash -- ver
    // el comentario sobre lumen_native_fail() en native_gen.cpp. El mensaje
    // no viaja en el propio NativeValue (no hay sitio en una union de 8
    // bytes) sino en un buffer por hilo dentro de la biblioteca, leido con
    // ErrorMessageFn justo despues de recibir este tag.
    enum class Tag : int32_t { Int, Float, Bool, Error } tag = Tag::Int;
    union {
        int64_t i;
        double  d;
        bool    b;
    };
};

// Firma unica de cualquier funcion --native ya cargada: los argumentos van
// empaquetados en `args[0..argc)`, en el mismo orden que los parametros
// Lumen; el resultado es un unico NativeValue.
using CompiledFn = NativeValue (*)(const NativeValue*, int32_t);

// Simbolo fijo que exporta toda biblioteca --native (ver
// native_gen.cpp::error_runtime_prelude()): el mensaje que corresponde al
// ultimo NativeValue::Tag::Error devuelto en el hilo que llama, valido hasta
// la siguiente llamada nativa en ese mismo hilo -- quien lo lee debe copiarlo
// a un std::string propio antes de hacer cualquier otra llamada nativa.
using ErrorMessageFn = const char* (*)();

// Lo que necesita la VM para despachar una llamada a codigo nativo: la tabla
// de punteros de invocacion (indexada por FnSig::index, igual que
// FunctionTable) y el accesor del mensaje de error. Un valor liviano,
// construido en cada punto de llamada a partir de un NativeModule (ver
// native_build.hpp) -- no posee nada, solo apunta.
struct NativeDispatch {
    const std::vector<CompiledFn>* funcs         = nullptr;
    ErrorMessageFn                 error_message = nullptr;
};

} // namespace lumen_script
