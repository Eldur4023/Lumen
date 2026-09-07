#pragma once
#include <string>
#include <vector>

#include "bytecode.hpp"
#include "diagnostic.hpp"
#include "emitter.hpp"
#include "natives.hpp"
#include "value.hpp"

namespace lumen_script {

// Motor de plantillas de Lumen Script.
//
// La forma es la de Jinja2 —`{{ }}`, `{% if %}`, `{% for %}`— porque es la que
// escribe la gente y la que generan los LLM.  Lo que va DENTRO son expresiones
// de Lumen Script, no un lenguaje aparte: `{{ a.titulo }}` es la misma expresion que
// escribirias en el .lum, parseada por el mismo parser y ejecutada por el
// mismo VM.
//
// Eso da lo que ningun motor externo puede dar: una errata en una plantilla es
// un error de COMPILACION, con fichero, linea y cursor, no un fallo en
// produccion.
//
// La plantilla se compila una vez al arrancar, como las rutas.  Renderizar es
// recorrer una lista de instrucciones muy simples: pegar un trozo de texto, o
// evaluar una expresion y pegar su resultado escapado.
struct Plantilla {
    enum class Op : uint8_t {
        Texto,          // pega textos[a]
        Escribir,       // evalua exprs[a] y pega el resultado, escapado
        EscribirCrudo,  // idem, sin escapar (marcado con |safe)
        SaltarSiFalso,  // evalua exprs[a]; si es falso, salta a b
        Saltar,         // salta a b
        BucleInicio,    // exprs[a] da la lista; si esta vacia salta a b
        BucleSiguiente, // siguiente vuelta: si queda, salta a b; si no, sigue
    };

    struct Instr {
        Op        op;
        uint32_t  a    = 0;
        uint32_t  b    = 0;
        uint32_t  slot = 0;   // ranura del item, en los bucles
        uint32_t  slot_loop = 0;
        SourceLoc loc;
    };

    std::vector<std::string> textos;
    std::vector<Chunk>       exprs;
    std::vector<Instr>       code;

    // Nombres de las ranuras, en orden.  Las primeras son los datos que la
    // ruta pasa a render(); las siguientes, las variables de los bucles.
    //
    // Llevan el tipo pegado porque las expresiones de dentro se compilan contra
    // ellos: es lo que permite que `{{ quien.mayusculas() }}` sobre un string
    // sea un error de compilacion y no una pagina rota.
    std::vector<NombreTipado> nombres;
};

// Compila el fuente de una plantilla.
//
// `datos` son los nombres —con su tipo, si se conoce— que la ruta va a pasar a
// render(), y ocupan las primeras ranuras.  `dir` es la carpeta de plantillas,
// para resolver {% include %}.  Devuelve false si hubo errores; van en `diags`.
bool compilar_plantilla(const std::string& fuente, const std::string& fichero,
                        const std::string& dir,
                        const std::vector<NombreTipado>& datos,
                        DiagnosticBag& diags, Plantilla& out);

// Renderiza.  `valores` llega en el mismo orden que los `datos` con los que se
// compilo.  Un error de ejecucion —dividir por cero dentro de un {{ }}— sale
// por `error` y deja `salida` a medias.
bool render_plantilla(const Plantilla& p, std::vector<Value> valores,
                      NativeCtx& ctx, const FunctionTable* fns,
                      std::string& salida, std::string& error);

// Escapa para HTML.  Publica porque la usa tambien el marcador |safe al
// decidir que NO escapar.
void escapar_html(const std::string& in, std::string& out);

} // namespace lumen_script
