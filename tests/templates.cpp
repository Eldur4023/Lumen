// Test of the Lumen Script template engine, with no HTTP involved.
#include <lumen_script/template.hpp>

#include <lumen/request.hpp>
#include <lumen/response.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using lumen_script::Template;
using lumen_script::Value;

static int         failures = 0;
static std::string dir_tpl = ".";

static void check(const char* name, const std::string& obtenido,
                      const std::string& expected) {
    if (obtenido == expected) {
        std::printf("  ok    %s\n", name);
    } else {
        ++failures;
        std::printf("  FAIL %s\n    expected: <<%s>>\n    got: <<%s>>\n",
                    name, expected.c_str(), obtenido.c_str());
    }
}

// Compiles and renders; returns "" and sets `err` if something fails.
static std::string pintar(const std::string& fuente,
                          const std::vector<lumen_script::TypedName>& names,
                          std::vector<Value> values, std::string& err) {
    lumen_script::DiagnosticBag diags;
    Template p;
    if (!lumen_script::compilar_plantilla(fuente, "test.html", dir_tpl, names, diags, p)) {
        err = diags.items().empty() ? "error without a message" : diags.items().front().message;
        return {};
    }
    lumen::Request  req;
    lumen::Response res;
    lumen_script::NativeCtx  ctx{req, res};

    std::string out;
    if (!lumen_script::render_plantilla(p, std::move(values), ctx, nullptr, out, err))
        return {};
    return out;
}

static void case_(const char* name, const std::string& fuente,
                 const std::vector<lumen_script::TypedName>& names,
                 std::vector<Value> values, const std::string& expected) {
    std::string err;
    const std::string got = pintar(fuente, names, std::move(values), err);
    if (!err.empty()) {
        ++failures;
        std::printf("  FAIL %s\n    unexpected error: %s\n", name, err.c_str());
        return;
    }
    check(name, got, expected);
}

// Cases that MUST fail to compile, with the expected reason.
static void fails_to_compile(const char* name, const std::string& fuente,
                       const std::vector<lumen_script::TypedName>& names,
                       const std::string& trozo) {
    std::string err;
    const std::string got = pintar(fuente, names, {}, err);
    if (err.empty()) {
        ++failures;
        std::printf("  FAIL %s\n    compiled when it should not: <<%s>>\n", name, got.c_str());
        return;
    }
    if (err.find(trozo) == std::string::npos) {
        ++failures;
        std::printf("  FAIL %s\n    expected the error to say '%s'\n    said: %s\n",
                    name, trozo.c_str(), err.c_str());
        return;
    }
    std::printf("  ok    %s\n", name);
}

int main() {
    std::printf("== text and expressions ==\n");
    case_("text suelto", "hola mundo", {}, {}, "hola mundo");
    case_("expresion simple", "<p>{{ n }}</p>", {"n"}, {Value::integer(42)},
         "<p>42</p>");
    case_("aritmetica", "{{ n * 2 + 1 }}", {"n"}, {Value::integer(20)}, "41");
    case_("string_value", "hola {{ quien }}", {"quien"}, {Value::str("Ana")}, "hola Ana");
    case_("ternario", "{{ n > 10 ? \"alto\" : \"bajo\" }}", {"n"},
         {Value::integer(20)}, "alto");
    case_("method", "{{ s.upper() }}", {"s"}, {Value::str("ana")}, "ANA");

    std::printf("== autoescapado ==\n");
    case_("escapes by default", "{{ s }}", {"s"},
         {Value::str("<script>alert('x')</script>")},
         "&lt;script&gt;alert(&#39;x&#39;)&lt;/script&gt;");
    case_("safe marker", "{{ s|safe }}", {"s"}, {Value::str("<b>ok</b>")},
         "<b>ok</b>");
    case_("ampersand y comillas", "{{ s }}", {"s"}, {Value::str("a & \"b\"")},
         "a &amp; &quot;b&quot;");

    std::printf("== miembros y fields ==\n");
    {
        Value::Dict d;
        d["titulo"] = Value::str("Hola");
        d["vistas"] = Value::integer(7);
        case_("field de dict", "{{ a.titulo }} ({{ a.vistas }})", {"a"},
             {Value::dict(std::move(d))}, "Hola (7)");
    }

    std::printf("== condicionales ==\n");
    case_("if verdadero", "{% if n > 5 %}grande{% endif %}", {"n"},
         {Value::integer(9)}, "grande");
    case_("if falso", "{% if n > 5 %}grande{% endif %}", {"n"},
         {Value::integer(1)}, "");
    case_("if else", "{% if n > 5 %}grande{% else %}pequeno{% endif %}", {"n"},
         {Value::integer(1)}, "pequeno");
    case_("elif", "{% if n > 100 %}enorme{% elif n > 5 %}grande{% else %}pequeno{% endif %}",
         {"n"}, {Value::integer(9)}, "grande");
    case_("elif ultimo", "{% if n > 100 %}enorme{% elif n > 5 %}grande{% else %}pequeno{% endif %}",
         {"n"}, {Value::integer(1)}, "pequeno");
    case_("lumen_script truthiness", "{% if s %}yes{% else %}empty{% endif %}", {"s"},
         {Value::str("")}, "empty");

    std::printf("== loops ==\n");
    {
        Value::List l;
        l.push_back(Value::str("a"));
        l.push_back(Value::str("b"));
        l.push_back(Value::str("c"));
        case_("for simple", "{% for x in xs %}[{{ x }}]{% endfor %}", {"xs"},
             {Value::list(l)}, "[a][b][c]");
        case_("for with loop.index", "{% for x in xs %}{{ loop.index }}:{{ x }} {% endfor %}",
             {"xs"}, {Value::list(l)}, "1:a 2:b 3:c ");
        case_("loop.first y last", "{% for x in xs %}{% if loop.first %}<{% endif %}{{ x }}{% if loop.last %}>{% endif %}{% endfor %}",
             {"xs"}, {Value::list(l)}, "<abc>");
        case_("empty for", "[{% for x in xs %}{{ x }}{% endfor %}]", {"xs"},
             {Value::list({})}, "[]");
        case_("if dentro de for", "{% for x in xs %}{% if x != \"b\" %}{{ x }}{% endif %}{% endfor %}",
             {"xs"}, {Value::list(l)}, "ac");
    }
    {
        // Nested, with shadowing of the same name.
        Value::List interna;
        interna.push_back(Value::integer(1));
        interna.push_back(Value::integer(2));
        Value::List externa;
        externa.push_back(Value::list(interna));
        externa.push_back(Value::list(interna));
        case_("nested loops", "{% for x in xs %}({% for x in x %}{{ x }}{% endfor %}){% endfor %}",
             {"xs"}, {Value::list(std::move(externa))}, "(12)(12)");
    }

    std::printf("== comentarios y espacios ==\n");
    case_("comment", "a{# this does not show #}b", {}, {}, "ab");
    case_("trim", "a   {%- if true -%}   b{% endif %}", {}, {}, "ab");

    std::printf("== errores de compilacion ==\n");
    fails_to_compile("unknown field in the syntax", "{{ }}", {}, "expression");
    fails_to_compile("missing closing", "{{ n ", {"n"}, "missing the closing");
    fails_to_compile("stray endif", "{% endif %}", {}, "without {% if %}");
    fails_to_compile("unclosed if", "{% if n %}x", {"n"}, "missing {% endif %}");
    fails_to_compile("unclosed for", "{% for x in xs %}", {"xs"}, "missing {% endfor %}");
    fails_to_compile("malformed for", "{% for x xs %}{% endfor %}", {"xs"}, "for x in list");
    fails_to_compile("unknown tag", "{% cosa %}", {}, "unknown tag");
    fails_to_compile("jinja filter", "{{ x|upper }}", {"x"}, "Lumen Script methods");
    fails_to_compile("variable that does not exist", "{{ noexiste }}", {}, "in the template expression");
    fails_to_compile("trailing garbage", "{{ n n }}", {"n"}, "trailing input");

    // ── Herencia ────────────────────────────────────────────────────────────
    // It needs real files: {% extends %} reads them from disk.
    {
        namespace fs = std::filesystem;
        const fs::path d = fs::temp_directory_path() / "lumen_script_tpl_prueba";
        fs::remove_all(d);
        fs::create_directories(d);
        dir_tpl = d.string();
        auto write = [&](const char* n, const char* t) { std::ofstream(d / n) << t; };
        auto read = [&](const char* n) {
            std::ifstream f(d / n);
            return std::string((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        };

        write("base.html",
                 "<html>{% block cabeza %}HEAD{% endblock %}"
                 "|{% block body %}empty{% endblock %}</html>");
        write("hijo.html",
                 "{% extends \"base.html\" %}{% block body %}soy {{ quien }}{% endblock %}");
        write("nieto.html",
                 "{% extends \"hijo.html\" %}{% block cabeza %}NUEVA{% endblock %}");
        write("anidado.html",
                 "<a>{% block fuera %}[{% block dentro %}d{% endblock %}]{% endblock %}</a>");
        write("hijo_anidado.html",
                 "{% extends \"anidado.html\" %}{% block dentro %}D{% endblock %}");

        std::printf("== inheritance ==\n");
        case_("hijo sustituye un bloque", read("hijo.html"), {"quien"},
             {Value::str("Ana")}, "<html>HEAD|soy Ana</html>");
        case_("without substitution the default shows", read("base.html"), {}, {},
             "<html>HEAD|empty</html>");
        case_("string_value de tres", read("nieto.html"), {"quien"},
             {Value::str("Ana")}, "<html>NUEVA|soy Ana</html>");
        case_("bloque dentro de bloque", read("hijo_anidado.html"), {}, {},
             "<a>[D]</a>");

        fails_to_compile("extends is not first", "hola{% extends \"base.html\" %}", {},
                   "must be the first thing");
        fails_to_compile("base that does not exist", "{% extends \"nada.html\" %}", {},
                   "base template not found");
        fails_to_compile("missing endblock", "{% block x %}unclosed", {},
                   "missing {% endblock %}");
        fails_to_compile("macro does not exist", "{% macro m() %}{% endmacro %}", {},
                   "no existe en Lumen Script");

        dir_tpl = ".";
        fs::remove_all(d);
    }

    // When render() knows the type of what it passes, the template is checked
    // against it: a typo inside a {{ }} stops being a broken page.
    std::printf("== tipos ==\n");
    case_("method that exists", "{{ s.upper().trim() }}", {{"s", "string"}},
         {Value::str(" ana ")}, "ANA");
    fails_to_compile("method that does not exist", "{{ s.mayusculas() }}", {{"s", "string"}},
               "have no method");
    fails_to_compile("method with too many arguments", "{{ s.upper(1) }}", {{"s", "string"}},
               "expects 0 argument(s)");
    fails_to_compile("method after chaining", "{{ s.upper().recortar() }}", {{"s", "string"}},
               "have no method");
    fails_to_compile("field on a number", "{{ n.field }}", {{"n", "int"}},
               "which has no fields");

    // With no type there is nothing to check, and that has to keep compiling:
    // it is what happens to the variable of a {% for %}.
    case_("no type means no checking", "{% for x in xs %}{{ x.upper() }}{% endfor %}",
         {"xs"}, {Value::list({Value::str("a")})}, "A");

    std::printf("\n%s\n", failures ? "THERE ARE FAILURES" : "all passing");
    return failures ? 1 : 0;
}
