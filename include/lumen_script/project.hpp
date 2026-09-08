#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <map>

#include <lumen/router.hpp>
#include "bytecode.hpp"
#include "ast.hpp"
#include "emitter.hpp"
#include "native_build.hpp"
#include "plantilla.hpp"
#include "diagnostic.hpp"

namespace lumen_script {

// Un modulo compilado: el resultado de leer un conjunto de .lum.
//
// Es la unidad que se intercambia en caliente.  Recargar es construir un
// Module nuevo y publicar el shared_ptr; si la compilacion falla, el anterior
// sigue en su sitio y no se toca nada.
struct Module {
    std::vector<std::unique_ptr<SourceFile>> files;
    Program        program;
    lumen::Router router;

    // Funciones de usuario compiladas, indexadas por orden de declaracion.
    FunctionTable functions;

    // Firmas de esas mismas funciones -- solo se guarda para poder ofrecerla
    // a compilar_nativo() sin tener que reconstruirla; el resto del modulo
    // ya no la necesita despues de compilar.
    FunctionSigs function_sigs;

    // --native (Fase 2 de COMPILACION-NATIVA.md): nulo salvo que se haya
    // pedido `--native` y al menos una funcion se haya podido compilar. Un
    // aviso no vacio en `native_aviso` es una degradacion parcial o total a
    // bytecode -- nunca un motivo para no publicar el modulo -- que quien
    // arranca el binario decide como mostrar.
    std::unique_ptr<NativeModule> native;
    std::string                   native_aviso;

    // Plantillas compiladas, indexadas por el orden en que el emisor las
    // encontro.  Cada render() del fuente tiene la suya, compilada contra las
    // claves concretas que le pasa esa llamada.
    std::vector<Plantilla> plantillas;

    // Especificacion OpenAPI generada desde el AST al compilar.
    std::string    openapi;      // ya serializado al compilar

    // Manejadores de `on error`, por codigo.  La clave 0 es el global.
    std::map<int, std::shared_ptr<Chunk>> error_handlers;

    // Reparto entre los dos niveles de ruta (ver LUMEN-2.0.md, seccion 2):
    // las declarativas no ejecutan ni un paso de bytecode.
    int declarative_routes = 0;
    int vm_routes          = 0;

    // Fase 6 de --native (COMPILACION-NATIVA.md): que via sirve CADA ruta,
    // en el mismo orden que Program::routes -- "diagnostico explicito de
    // rutas no compilables" en vez de solo el conteo agregado que ya daba
    // el arranque. Se rellena siempre (con --native o sin el), aunque solo
    // main.cpp la imprime cuando --native esta activo: sin --native todas
    // las rutas con logica llevan "bytecode" y el conteo agregado ya basta.
    struct RutaInforme {
        std::string metodo;
        std::string patron;
        std::string via; // "declarativa" | "nativa" | "nativa (async)" | "bytecode" | "ws" | "sse"
    };
    std::vector<RutaInforme> rutas_informe;

    // mtimes de los ficheros compilados, para detectar cambios.
    std::vector<std::pair<std::filesystem::path,
                          std::filesystem::file_time_type>> stamps;
};

// Resuelve los argumentos de linea de comandos al conjunto de ficheros a
// compilar:
//   • un fichero  → solo ese
//   • varios      → solo esos
//   • un directorio → todos los .lum que contenga, recursivamente
//
// Devuelve false y escribe en `error` si algun argumento no existe o si un
// directorio no contiene ningun .lum.
bool resolve_inputs(const std::vector<std::string>& args,
                    std::vector<std::filesystem::path>& out,
                    std::string& error);

// Lee, lexa, parsea y construye la tabla de rutas.
//
// SIEMPRE devuelve un Module, incluso si hubo errores: los SourceLoc apuntan a
// los SourceFile que este Module posee, asi que destruirlo antes de formatear
// los diagnosticos dejaria punteros colgando.  El exito se comprueba con
// `diags.empty()`, y un modulo con errores simplemente no se publica.
//
// `native`, si es verdad, ademas intenta compilar a codigo nativo (Fase 2 de
// --native) las funciones (`fn`) del programa que lo permitan, DESPUES de
// que el resto compile sin errores -- nunca es lo que hace fallar
// `diags.empty()`: un aviso queda en `Module::native_aviso` y las funciones
// que no se pudieron compilar se sirven con bytecode, igual que si
// `native` fuera falso.
std::shared_ptr<Module> compile(const std::vector<std::filesystem::path>& inputs,
                                DiagnosticBag& diags, bool native = false);

// Formatea los diagnosticos de un intento fallido usando los ficheros leidos.
std::string format_errors(const DiagnosticBag& diags,
                          const std::vector<std::unique_ptr<SourceFile>>& files);

} // namespace lumen_script
