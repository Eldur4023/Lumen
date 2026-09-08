#pragma once
#include <cstdint>

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
    enum class Tag : int32_t { Int, Float, Bool } tag = Tag::Int;
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

} // namespace lumen_script
