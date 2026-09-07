// Smoke test del motor tras la cirugía de Lumen 2.0.
// Comprueba las tres cosas que el hito 0 tenía que dejar en pie:
// ficheros estáticos, plantillas Jinja2 y una ruta que devuelve JSON.
#include <lumen/lumen.hpp>

using namespace lumen;

int main() {
    App app;
    app.use(lumen::logger());
    app.set_templates("./templates");

    app.get("/json", [](Response& res) {
        res.json_text(R"({"ok":true,"engine":"lumen-2.0"})");
    });

    // Las plantillas ya no se renderizan desde C++: viven en Lumen Script, se compilan
    // al arrancar y se piden con render() desde un .lum.  Ver ejemplo/.

    app.serve_static("/static", "./public");

    app.run(8080);
}
