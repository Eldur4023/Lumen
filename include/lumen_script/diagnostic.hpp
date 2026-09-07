#pragma once
#include <string>
#include <vector>
#include "token.hpp"

namespace lumen_script {

// Un fichero fuente ya leido.  `path` vive aqui y los SourceLoc apuntan a el,
// asi que un SourceFile no puede moverse mientras haya tokens vivos.
struct SourceFile {
    std::string path;
    std::string text;

    // Devuelve la linea `n` (1-indexada) sin el salto final.
    std::string_view line(int n) const;
};

struct Diagnostic {
    SourceLoc   loc;
    std::string message;
};

// Recoge errores en vez de abortar al primero: un fichero con tres fallos debe
// reportar los tres, no obligar a compilar tres veces.
class DiagnosticBag {
public:
    void error(SourceLoc loc, std::string message) {
        items_.push_back({loc, std::move(message)});
    }

    bool empty()  const { return items_.empty(); }
    size_t size() const { return items_.size(); }
    const std::vector<Diagnostic>& items() const { return items_; }

    void clear() { items_.clear(); }

    // Formatea todos los diagnosticos con fichero:linea:columna, la linea de
    // codigo y un cursor bajo la posicion exacta.  `files` se usa para
    // recuperar el texto de cada linea.
    std::string format(const std::vector<const SourceFile*>& files) const;

private:
    std::vector<Diagnostic> items_;
};

} // namespace lumen_script
