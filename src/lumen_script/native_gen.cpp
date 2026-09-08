#include <lumen_script/native_gen.hpp>

#include <map>
#include <sstream>

namespace lumen_script {
namespace {

// Los unicos Type que esta fase sabe representar de forma nativa -- ver la
// tabla de §7. `?` (optional) haria falta empaquetarlo (std::optional<T> o
// un centinela) y esta fase no lo cubre todavia: una funcion con un
// parametro/campo/retorno opcional se queda en bytecode por ahora.
bool tipo_soportado(const Type& t) {
    if (t.is_optional()) return false;
    switch (t.kind()) {
        case Type::Kind::Int:
        case Type::Kind::Float:
        case Type::Kind::Bool:
        case Type::Kind::Void:
            return true;
        default:
            return false;
    }
}

std::string tipo_cpp(const Type& t) {
    switch (t.kind()) {
        case Type::Kind::Int:   return "int64_t";
        case Type::Kind::Float: return "double";
        case Type::Kind::Bool:  return "bool";
        case Type::Kind::Void:  return "void";
        default: return ""; // inalcanzable si tipo_soportado() dio el visto bueno
    }
}

// Prefijo fijo para todo identificador generado (variables, parametros,
// funciones): evita por construccion cualquier choque con una palabra
// reservada de C++ que no lo sea de Lumen Script (`new`, `class`,
// `template`...), sin necesitar una lista de palabras reservadas que
// mantener sincronizada con el estandar.
std::string nombre_cpp(const std::string& lumen) { return "l_" + lumen; }

// Ver native_abi.hpp: campo de la union de NativeValue que corresponde a
// cada Type::Kind soportado, y la etiqueta que hay que ponerle.
std::string campo_abi(Type::Kind k) {
    switch (k) {
        case Type::Kind::Int:   return "i";
        case Type::Kind::Float: return "d";
        case Type::Kind::Bool:  return "b";
        default: return ""; // inalcanzable: tipo_soportado() ya lo descarto
    }
}

std::string etiqueta_abi(Type::Kind k) {
    switch (k) {
        case Type::Kind::Int:   return "NativeValue::Tag::Int";
        case Type::Kind::Float: return "NativeValue::Tag::Float";
        case Type::Kind::Bool:  return "NativeValue::Tag::Bool";
        default: return "";
    }
}

// ── Comprobacion: ¿esta esto enteramente dentro de lo que compila esta fase? ──

bool expr_compilable(const IrExpr& e);

bool expr_compilable_ptr(const IrExpr* e) { return e && expr_compilable(*e); }

bool expr_compilable(const IrExpr& e) {
    switch (e.kind) {
        case IrExprKind::IntLit:
        case IrExprKind::FloatLit:
        case IrExprKind::BoolLit:
            return true;

        // Sin representacion en esta fase (cadenas, contenedores, Json) o
        // sin sentido fuera de una ruta/clase (this, un miembro, await).
        case IrExprKind::NullLit:
        case IrExprKind::StringLit:
        case IrExprKind::ListLit:
        case IrExprKind::DictLit:
        case IrExprKind::Member:
        case IrExprKind::Index:
        case IrExprKind::Await:
        case IrExprKind::This:
            return false;

        case IrExprKind::Ident:
            return tipo_soportado(e.type);

        case IrExprKind::Unary:
            return expr_compilable_ptr(e.lhs.get());

        case IrExprKind::Binary:
            return expr_compilable_ptr(e.lhs.get()) && expr_compilable_ptr(e.rhs.get());

        case IrExprKind::Ternary:
            return expr_compilable_ptr(e.object.get()) && expr_compilable_ptr(e.lhs.get()) &&
                   expr_compilable_ptr(e.rhs.get());

        // Solo sobre una variable: un Member/Index como objetivo ya implica
        // una clase o un contenedor, fuera de esta fase.
        case IrExprKind::PreStep:
        case IrExprKind::PostStep:
            return e.lhs && e.lhs->kind == IrExprKind::Ident;

        // Solo llamadas a otra funcion de usuario, con argumentos igual de
        // compilables. Las otras 7 formas de IrCallShape (builtins, metodos,
        // BD, objetos reservados) no tienen contrapartida nativa todavia.
        case IrExprKind::Call:
            if (e.call_shape != IrCallShape::UserFunctionCall) return false;
            for (const auto& a : e.args)
                if (!expr_compilable_ptr(a.value.get())) return false;
            return true;
    }
    return false;
}

bool stmt_compilable(const IrStmt& s);

bool block_compilable(const IrBlock& b) {
    for (const auto& s : b)
        if (!s || !stmt_compilable(*s)) return false;
    return true;
}

bool stmt_compilable(const IrStmt& s) {
    switch (s.kind) {
        case IrStmtKind::Return:
            return !s.value || expr_compilable(*s.value);

        case IrStmtKind::ExprStmt:
            return s.value && expr_compilable(*s.value);

        case IrStmtKind::VarDecl:
            return tipo_soportado(s.decl_type) && (!s.value || expr_compilable(*s.value));

        // Solo la forma Local: Session/Index/Member implican sesion, un
        // contenedor o una clase.
        case IrStmtKind::Assign:
            return s.assign_target == IrAssignTarget::Local &&
                   s.value && expr_compilable(*s.value);

        case IrStmtKind::If:
            return s.value && expr_compilable(*s.value) &&
                   block_compilable(s.body) && block_compilable(s.orelse);

        case IrStmtKind::While:
            return s.value && expr_compilable(*s.value) && block_compilable(s.body);

        case IrStmtKind::Break:
        case IrStmtKind::Continue:
            return true;

        // For itera una List (contenedor). Require y Try no son "primitivos
        // y control de flujo" en el sentido estrecho de esta fase todavia
        // -- Try en concreto necesita decidir como se representa un error
        // nativo, que es una decision de la fase 5 (asincronia y errores).
        case IrStmtKind::For:
        case IrStmtKind::Require:
        case IrStmtKind::Try:
            return false;
    }
    return false;
}

// ── Generacion ────────────────────────────────────────────────────────────

// Formato inequivoco: siempre con punto decimal, para que "1.0" no se
// genere como el entero "1" (que en C++ convierte implicitamente pero deja
// de ser, a simple vista, el literal de coma flotante que era en el .lum).
std::string literal_float(double d) {
    std::ostringstream o;
    o.precision(17);
    o << d;
    std::string s = o.str();
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
        s += ".0";
    return s;
}

const std::map<std::string, std::string>& operadores_binarios() {
    static const std::map<std::string, std::string> ops = {
        {"+", "+"}, {"-", "-"}, {"*", "*"}, {"/", "/"}, {"%", "%"},
        {"==", "=="}, {"!=", "!="}, {"<", "<"}, {"<=", "<="}, {">", ">"}, {">=", ">="},
        {"and", "&&"}, {"or", "||"},
    };
    return ops;
}

class Generador {
public:
    explicit Generador(const std::vector<std::string>& nombre_por_indice)
        : nombre_por_indice_(nombre_por_indice) {}

    // Ranura -> nombre C++ ya calculado, para poder generar `nombre = ...`
    // en un Assign(Local): el IrStmt solo trae `assign_slot` (lo unico que
    // necesita el bytecode), no un nombre, asi que el generador lleva su
    // propia cuenta segun va viendo parametros y VarDecl. Los parametros se
    // registran en generar_funcion_nativa (ranura i-esima = parametro
    // i-esimo, por como los declara check_function antes que nada mas).
    void registrar(int slot, const std::string& nombre) { ranura_a_nombre_[slot] = nombre; }

    std::string expr(const IrExpr& e) const {
        switch (e.kind) {
            case IrExprKind::IntLit:   return std::to_string(e.int_value) + "LL";
            case IrExprKind::FloatLit: return literal_float(e.float_value);
            case IrExprKind::BoolLit:  return e.bool_value ? "true" : "false";
            case IrExprKind::Ident:    return nombre_cpp(e.text);

            case IrExprKind::Unary:
                return std::string("(") + (e.text == "not" ? "!" : "-") + expr(*e.lhs) + ")";

            case IrExprKind::Binary:
                return "(" + expr(*e.lhs) + " " + operadores_binarios().at(e.text) + " " +
                       expr(*e.rhs) + ")";

            case IrExprKind::Ternary:
                return "(" + expr(*e.object) + " ? " + expr(*e.lhs) + " : " + expr(*e.rhs) + ")";

            case IrExprKind::PreStep:
            case IrExprKind::PostStep: {
                const std::string op = (e.text == "+") ? "++" : "--";
                const std::string v  = nombre_cpp(e.lhs->text);
                return e.kind == IrExprKind::PreStep ? ("(" + op + v + ")") : ("(" + v + op + ")");
            }

            case IrExprKind::Call: {
                std::string s = nombre_cpp(nombre_por_indice_.at(static_cast<size_t>(e.call_index)));
                s += "(";
                for (size_t i = 0; i < e.args.size(); ++i) {
                    if (i) s += ", ";
                    s += expr(*e.args[i].value);
                }
                s += ")";
                return s;
            }

            default:
                return ""; // inalcanzable: expr_compilable() ya lo descarto antes de llegar aqui
        }
    }

    std::string block(const IrBlock& b, int indent) {
        std::string s;
        const std::string p(static_cast<size_t>(indent) * 4, ' ');
        for (const auto& st : b) s += p + stmt(*st, indent) + "\n";
        return s;
    }

    std::string stmt(const IrStmt& s, int indent) {
        switch (s.kind) {
            case IrStmtKind::Return:
                return s.value ? ("return " + expr(*s.value) + ";") : "return;";

            case IrStmtKind::ExprStmt:
                return expr(*s.value) + ";";

            case IrStmtKind::VarDecl: {
                registrar(s.slot, s.name);
                return tipo_cpp(s.decl_type) + " " + nombre_cpp(s.name) + " = " +
                       (s.value ? expr(*s.value) : valor_por_defecto(s.decl_type)) + ";";
            }

            // stmt_compilable() ya garantizo assign_target == Local: el
            // nombre C++ es el que se registro cuando esa ranura se declaro
            // (un parametro o un VarDecl anterior).
            case IrStmtKind::Assign:
                return nombre_cpp(ranura_a_nombre_.at(s.assign_slot)) + " = " +
                       expr(*s.value) + ";";

            case IrStmtKind::If: {
                std::string r = "if (" + expr(*s.value) + ") {\n" + block(s.body, indent + 1) +
                                pad(indent) + "}";
                if (!s.orelse.empty())
                    r += " else {\n" + block(s.orelse, indent + 1) + pad(indent) + "}";
                return r;
            }

            case IrStmtKind::While:
                return "while (" + expr(*s.value) + ") {\n" + block(s.body, indent + 1) +
                       pad(indent) + "}";

            case IrStmtKind::Break:    return "break;";
            case IrStmtKind::Continue: return "continue;";

            default:
                return ""; // inalcanzable: stmt_compilable() ya lo descarto antes de llegar aqui
        }
    }

private:
    const std::vector<std::string>& nombre_por_indice_;
    std::map<int, std::string>      ranura_a_nombre_;

    static std::string pad(int indent) { return std::string(static_cast<size_t>(indent) * 4, ' '); }

    static std::string valor_por_defecto(const Type& t) {
        switch (t.kind()) {
            case Type::Kind::Int:   return "0";
            case Type::Kind::Float: return "0.0";
            case Type::Kind::Bool:  return "false";
            default:                return "{}";
        }
    }
};

} // namespace

std::optional<FuncionNativa> generar_funcion_nativa(const FnDecl& fn, const IrBlock& body,
                                                     const std::vector<std::string>& nombre_por_indice) {
    if (!tipo_soportado(Type::from_declared(fn.return_type))) return std::nullopt;
    for (const auto& p : fn.params)
        if (!tipo_soportado(Type::from_declared(p.type))) return std::nullopt;
    if (!block_compilable(body)) return std::nullopt;

    FuncionNativa out;
    out.nombre_lumen = fn.name;

    std::string params;
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (i) params += ", ";
        params += tipo_cpp(Type::from_declared(fn.params[i].type)) + " " +
                  nombre_cpp(fn.params[i].name);
    }
    out.firma_cpp = tipo_cpp(Type::from_declared(fn.return_type)) + " " + nombre_cpp(fn.name) +
                    "(" + params + ")";

    Generador gen(nombre_por_indice);
    // check_function declara los parametros, en orden, antes que nada mas
    // (ver Emitter::check_function): la ranura i-esima es siempre el
    // parametro i-esimo.
    for (size_t i = 0; i < fn.params.size(); ++i)
        gen.registrar(static_cast<int>(i), fn.params[i].name);
    out.cuerpo_cpp = "{\n" + gen.block(body, 1) + "}";

    // El wrapper de ABI fija (ver native_abi.hpp): descomprime args[i] al
    // tipo real de cada parametro, llama a la funcion de arriba, y empaqueta
    // el resultado -- o, si la funcion es void, un NativeValue::Tag::Int a 0
    // que nadie mira (el llamante conoce el tipo de retorno declarado, igual
    // que ya conoce la aridad, asi que un valor void nunca se desempaqueta).
    out.simbolo_abi = "lumen_native_" + fn.name;
    std::string cuerpo_wrapper = "    (void)argc;\n";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        const Type t = Type::from_declared(fn.params[i].type);
        cuerpo_wrapper += "    " + tipo_cpp(t) + " " + nombre_cpp(fn.params[i].name) +
                          " = args[" + std::to_string(i) + "]." + campo_abi(t.kind()) + ";\n";
    }
    const Type ret = Type::from_declared(fn.return_type);
    const std::string llamada = nombre_cpp(fn.name) + "(" + [&] {
        std::string s;
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (i) s += ", ";
            s += nombre_cpp(fn.params[i].name);
        }
        return s;
    }() + ")";
    if (ret.kind() == Type::Kind::Void) {
        cuerpo_wrapper += "    " + llamada + ";\n"
                          "    NativeValue salida; salida.tag = NativeValue::Tag::Int; salida.i = 0;\n"
                          "    return salida;\n}";
    } else {
        cuerpo_wrapper += "    NativeValue salida; salida.tag = " + etiqueta_abi(ret.kind()) +
                          "; salida." + campo_abi(ret.kind()) + " = " + llamada + ";\n"
                          "    return salida;\n}";
    }
    out.wrapper_cpp = "extern \"C\" NativeValue " + out.simbolo_abi +
                      "(const NativeValue* args, int32_t argc) {\n" +
                      cuerpo_wrapper;
    return out;
}

std::string abi_prelude() {
    // Identico, campo a campo, a la definicion de include/lumen_script/native_abi.hpp.
    return
        "struct NativeValue {\n"
        "    enum class Tag : int32_t { Int, Float, Bool } tag = Tag::Int;\n"
        "    union { int64_t i; double d; bool b; };\n"
        "};\n"
        "using NativeFn = NativeValue (*)(const NativeValue*, int32_t);\n";
}

} // namespace lumen_script
