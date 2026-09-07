// Smoke test of the engine after the Lumen surgery.
// It checks the three things milestone 0 had to leave standing:
// static files, Jinja2 templates and a route returning JSON.
#include <lumen/lumen.hpp>

using namespace lumen;

int main() {
    App app;
    app.use(lumen::logger());
    app.set_templates("./templates");

    app.get("/json", [](Response& res) {
        res.json_text(R"({"ok":true,"engine":"lumen-2.0"})");
    });

    // Templates are no longer rendered from C++: they live in Lumen Script,
    // at startup and are requested with render() from a .lum.  See example/.

    app.serve_static("/static", "./public");

    app.run(8080);
}
