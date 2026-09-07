// Prueba del motor de plantillas de Lumen Script, sin HTTP de por medio.
#include <lumen_script/plantilla.hpp>

#include <lumen/request.hpp>
#include <lumen/response.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using lumen_script::Plantilla;
using lumen_script::Value;

static int         fallos = 0;
static std::string dir_tpl = ".";

static void comprobar(const char* nombre, const std::string& obtenido,
                      const std::string& esperado) {
    if (obtenido == esperado) {
        std::printf("  ok    %s\n", nombre);
    } else {
        ++fallos;
        std::printf("  FALLA %s\n    esperado: <<%s>>\n    obtenido: <<%s>>\n",
                    nombre, esperado.c_str(), obtenido.c_str());
    }
}

// Compila y renderiza; devuelve "" y deja `err` si algo falla.
static std::string pintar(const std::string& fuente,
                          const std::vector<lumen_script::NombreTipado>& nombres,
                          std::vector<Value> valores, std::string& err) {
    lumen_script::DiagnosticBag diags;
    Plantilla p;
    if (!lumen_script::compilar_plantilla(fuente, "prueba.html", dir_tpl, nombres, diags, p)) {
        err = diags.items().empty() ? "error sin mensaje" : diags.items().front().message;
        return {};
    }
    lumen::Request  req;
    lumen::Response res;
    lumen_script::NativeCtx  ctx{req, res};

    std::string salida;
    if (!lumen_script::render_plantilla(p, std::move(valores), ctx, nullptr, salida, err))
        return {};
    return salida;
}

static void caso(const char* nombre, const std::string& fuente,
                 const std::vector<lumen_script::NombreTipado>& nombres,
                 std::vector<Value> valores, const std::string& esperado) {
    std::string err;
    const std::string got = pintar(fuente, nombres, std::move(valores), err);
    if (!err.empty()) {
        ++fallos;
        std::printf("  FALLA %s\n    error inesperado: %s\n", nombre, err.c_str());
        return;
    }
    comprobar(nombre, got, esperado);
}

// Casos que TIENEN que fallar al compilar, con el motivo esperado.
static void no_compila(const char* nombre, const std::string& fuente,
                       const std::vector<lumen_script::NombreTipado>& nombres,
                       const std::string& trozo) {
    std::string err;
    const std::string got = pintar(fuente, nombres, {}, err);
    if (err.empty()) {
        ++fallos;
        std::printf("  FALLA %s\n    compilo cuando no debia: <<%s>>\n", nombre, got.c_str());
        return;
    }
    if (err.find(trozo) == std::string::npos) {
        ++fallos;
        std::printf("  FALLA %s\n    esperaba que el error dijera '%s'\n    dijo: %s\n",
                    nombre, trozo.c_str(), err.c_str());
        return;
    }
    std::printf("  ok    %s\n", nombre);
}

int main() {
    std::printf("== texto y expresiones ==\n");
    caso("texto suelto", "hola mundo", {}, {}, "hola mundo");
    caso("expresion simple", "<p>{{ n }}</p>", {"n"}, {Value::integer(42)},
         "<p>42</p>");
    caso("aritmetica", "{{ n * 2 + 1 }}", {"n"}, {Value::integer(20)}, "41");
    caso("cadena", "hola {{ quien }}", {"quien"}, {Value::str("Ana")}, "hola Ana");
    caso("ternario", "{{ n > 10 ? \"alto\" : \"bajo\" }}", {"n"},
         {Value::integer(20)}, "alto");
    caso("metodo", "{{ s.upper() }}", {"s"}, {Value::str("ana")}, "ANA");

    std::printf("== autoescapado ==\n");
    caso("escapa por defecto", "{{ s }}", {"s"},
         {Value::str("<script>alert('x')</script>")},
         "&lt;script&gt;alert(&#39;x&#39;)&lt;/script&gt;");
    caso("marcador safe", "{{ s|safe }}", {"s"}, {Value::str("<b>ok</b>")},
         "<b>ok</b>");
    caso("ampersand y comillas", "{{ s }}", {"s"}, {Value::str("a & \"b\"")},
         "a &amp; &quot;b&quot;");

    std::printf("== miembros y campos ==\n");
    {
        Value::Dict d;
        d["titulo"] = Value::str("Hola");
        d["vistas"] = Value::integer(7);
        caso("campo de dict", "{{ a.titulo }} ({{ a.vistas }})", {"a"},
             {Value::dict(std::move(d))}, "Hola (7)");
    }

    std::printf("== condicionales ==\n");
    caso("if verdadero", "{% if n > 5 %}grande{% endif %}", {"n"},
         {Value::integer(9)}, "grande");
    caso("if falso", "{% if n > 5 %}grande{% endif %}", {"n"},
         {Value::integer(1)}, "");
    caso("if else", "{% if n > 5 %}grande{% else %}pequeno{% endif %}", {"n"},
         {Value::integer(1)}, "pequeno");
    caso("elif", "{% if n > 100 %}enorme{% elif n > 5 %}grande{% else %}pequeno{% endif %}",
         {"n"}, {Value::integer(9)}, "grande");
    caso("elif ultimo", "{% if n > 100 %}enorme{% elif n > 5 %}grande{% else %}pequeno{% endif %}",
         {"n"}, {Value::integer(1)}, "pequeno");
    caso("veracidad de lumen_script", "{% if s %}hay{% else %}vacio{% endif %}", {"s"},
         {Value::str("")}, "vacio");

    std::printf("== bucles ==\n");
    {
        Value::List l;
        l.push_back(Value::str("a"));
        l.push_back(Value::str("b"));
        l.push_back(Value::str("c"));
        caso("for simple", "{% for x in xs %}[{{ x }}]{% endfor %}", {"xs"},
             {Value::list(l)}, "[a][b][c]");
        caso("for con loop.index", "{% for x in xs %}{{ loop.index }}:{{ x }} {% endfor %}",
             {"xs"}, {Value::list(l)}, "1:a 2:b 3:c ");
        caso("loop.first y last", "{% for x in xs %}{% if loop.first %}<{% endif %}{{ x }}{% if loop.last %}>{% endif %}{% endfor %}",
             {"xs"}, {Value::list(l)}, "<abc>");
        caso("for vacio", "[{% for x in xs %}{{ x }}{% endfor %}]", {"xs"},
             {Value::list({})}, "[]");
        caso("if dentro de for", "{% for x in xs %}{% if x != \"b\" %}{{ x }}{% endif %}{% endfor %}",
             {"xs"}, {Value::list(l)}, "ac");
    }
    {
        // Anidado, con sombreado del mismo nombre.
        Value::List interna;
        interna.push_back(Value::integer(1));
        interna.push_back(Value::integer(2));
        Value::List externa;
        externa.push_back(Value::list(interna));
        externa.push_back(Value::list(interna));
        caso("bucles anidados", "{% for x in xs %}({% for x in x %}{{ x }}{% endfor %}){% endfor %}",
             {"xs"}, {Value::list(std::move(externa))}, "(12)(12)");
    }

    std::printf("== comentarios y espacios ==\n");
    caso("comentario", "a{# esto no sale #}b", {}, {}, "ab");
    caso("recorte", "a   {%- if true -%}   b{% endif %}", {}, {}, "ab");

    std::printf("== errores de compilacion ==\n");
    no_compila("campo inexistente en la sintaxis", "{{ }}", {}, "expresion");
    no_compila("cierre que falta", "{{ n ", {"n"}, "falta el cierre");
    no_compila("endif suelto", "{% endif %}", {}, "sin {% if %}");
    no_compila("if sin cerrar", "{% if n %}x", {"n"}, "falta {% endif %}");
    no_compila("for sin cerrar", "{% for x in xs %}", {"xs"}, "falta {% endfor %}");
    no_compila("for mal escrito", "{% for x xs %}{% endfor %}", {"xs"}, "for x in lista");
    no_compila("etiqueta desconocida", "{% cosa %}", {}, "etiqueta desconocida");
    no_compila("filtro de jinja", "{{ x|upper }}", {"x"}, "metodos de Lumen Script");
    no_compila("variable que no existe", "{{ noexiste }}", {}, "en la expresion");
    no_compila("basura detras", "{{ n n }}", {"n"}, "sobra algo");

    // ── Herencia ────────────────────────────────────────────────────────────
    // Necesita ficheros de verdad: {% extends %} los lee del disco.
    {
        namespace fs = std::filesystem;
        const fs::path d = fs::temp_directory_path() / "lumen_script_tpl_prueba";
        fs::remove_all(d);
        fs::create_directories(d);
        dir_tpl = d.string();
        auto escribir = [&](const char* n, const char* t) { std::ofstream(d / n) << t; };
        auto leer = [&](const char* n) {
            std::ifstream f(d / n);
            return std::string((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        };

        escribir("base.html",
                 "<html>{% block cabeza %}CABEZA{% endblock %}"
                 "|{% block cuerpo %}vacio{% endblock %}</html>");
        escribir("hijo.html",
                 "{% extends \"base.html\" %}{% block cuerpo %}soy {{ quien }}{% endblock %}");
        escribir("nieto.html",
                 "{% extends \"hijo.html\" %}{% block cabeza %}NUEVA{% endblock %}");
        escribir("anidado.html",
                 "<a>{% block fuera %}[{% block dentro %}d{% endblock %}]{% endblock %}</a>");
        escribir("hijo_anidado.html",
                 "{% extends \"anidado.html\" %}{% block dentro %}D{% endblock %}");

        std::printf("== herencia ==\n");
        caso("hijo sustituye un bloque", leer("hijo.html"), {"quien"},
             {Value::str("Ana")}, "<html>CABEZA|soy Ana</html>");
        caso("sin sustituir sale el defecto", leer("base.html"), {}, {},
             "<html>CABEZA|vacio</html>");
        caso("cadena de tres", leer("nieto.html"), {"quien"},
             {Value::str("Ana")}, "<html>NUEVA|soy Ana</html>");
        caso("bloque dentro de bloque", leer("hijo_anidado.html"), {}, {},
             "<a>[D]</a>");

        no_compila("extends no es lo primero", "hola{% extends \"base.html\" %}", {},
                   "tiene que ser lo primero");
        no_compila("base que no existe", "{% extends \"nada.html\" %}", {},
                   "no se encuentra la plantilla base");
        no_compila("endblock que falta", "{% block x %}sin cerrar", {},
                   "falta {% endblock %}");
        no_compila("macro no existe", "{% macro m() %}{% endmacro %}", {},
                   "no existe en Lumen Script");

        dir_tpl = ".";
        fs::remove_all(d);
    }

    // Cuando render() sabe el tipo de lo que pasa, la plantilla se comprueba
    // contra el: una errata dentro de un {{ }} deja de ser una pagina rota.
    std::printf("== tipos ==\n");
    caso("metodo que existe", "{{ s.upper().trim() }}", {{"s", "string"}},
         {Value::str(" ana ")}, "ANA");
    no_compila("metodo que no existe", "{{ s.mayusculas() }}", {{"s", "string"}},
               "no tienen el metodo");
    no_compila("metodo con argumentos de mas", "{{ s.upper(1) }}", {{"s", "string"}},
               "espera 0 argumento(s)");
    no_compila("metodo tras encadenar", "{{ s.upper().recortar() }}", {{"s", "string"}},
               "no tienen el metodo");
    no_compila("campo sobre un numero", "{{ n.campo }}", {{"n", "int"}},
               "no tiene campos");

    // Sin tipo no hay nada que comprobar, y eso tiene que seguir compilando:
    // es lo que le pasa a la variable de un {% for %}.
    caso("sin tipo no se comprueba", "{% for x in xs %}{{ x.upper() }}{% endfor %}",
         {"xs"}, {Value::list({Value::str("a")})}, "A");

    std::printf("\n%s\n", fallos ? "HAY FALLOS" : "todas pasan");
    return fallos ? 1 : 0;
}
