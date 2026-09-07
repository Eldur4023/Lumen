#pragma once
#include <memory>
#include <string>

#include "project.hpp"

namespace lumen_script {

// Sonda automatica de endpoints.
//
// Tras arrancar y tras cada recarga con exito, Lumen se pega a si mismo por
// HTTP y recorre las rutas del modulo.  No comprueba logica de negocio: lo que
// busca es que ningun handler se rompa —un 5xx— despues de un cambio.
//
// Sobre los efectos secundarios: probar un endpoint EJECUTA su handler.  Un
// DELETE o un POST harian su trabajo de verdad en cada recarga, asi que por
// defecto solo se recorren los metodos seguros (GET y HEAD).  El resto entra
// con `--autotest=all`, que es una decision de quien lo lanza y no del binario.
struct AutotestOptions {
    bool     enabled       = false;
    bool     unsafe        = false;   // incluir POST/PUT/PATCH/DELETE
    uint16_t port          = 8080;
    int      timeout_ms    = 3000;
    int      stream_ms     = 400;     // cuanto se escucha una ruta sse
};

// Recorre las rutas del modulo e imprime el resultado.  Bloquea, asi que se
// llama desde un hilo aparte del event loop.
void run_autotest(const Module& mod, const AutotestOptions& opts);

} // namespace lumen_script
