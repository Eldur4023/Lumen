#include <lumen_script/native_gen.hpp>

#include <cstdio>
#include <map>
#include <sstream>

namespace lumen_script {
namespace {

// Los unicos Type que esta fase sabe representar de forma nativa -- ver la
// tabla de §7. `?` (optional) haria falta empaquetarlo (std::optional<T> o
// un centinela) y esta fase no lo cubre todavia: una funcion con un
// parametro/campo/retorno opcional se queda en bytecode por ahora.
//
// `string` entra aqui como std::string por VALOR, no con el refcount
// intrusivo que describe §8 para listas/diccionarios/instancias -- porque en
// Lumen Script una cadena es inmutable (concatenar produce una cadena nueva,
// nunca muta la existente), asi que compartirla o copiarla es exactamente lo
// mismo desde fuera: no hay manera de observar la diferencia. Compartir por
// refcount es una optimizacion de rendimiento reservada para cuando haga
// falta (la Fase 7 aplica el mismo razonamiento a clases, mediante analisis
// de escape); por ahora, correcto antes que rapido.
bool tipo_soportado(const Type& t) {
    if (t.is_optional()) return false;
    switch (t.kind()) {
        case Type::Kind::Int:
        case Type::Kind::Float:
        case Type::Kind::Bool:
        case Type::Kind::Void:
        case Type::Kind::String:
            return true;
        default:
            return false;
    }
}

bool es_numerico(Type::Kind k) { return k == Type::Kind::Int || k == Type::Kind::Float; }

// Subconjunto de tipo_soportado() que puede cruzar la ABI fija de
// native_abi.hpp (NativeValue solo tiene un int64_t/double/bool en su
// union): una funcion cuyos parametros y retorno caen todos aqui puede
// recibir un wrapper `extern "C"` y ser invocada desde la VM; una que use
// `string` en su frontera todavia no -- pero SI se genera su cuerpo C++
// (ver generar_funcion_nativa), asi que otra funcion nativa que la llame
// directamente (sin pasar por la ABI) se beneficia igual. Extender la ABI
// para que tambien lleve cadenas queda para cuando una funcion con `string`
// en la frontera sea, ella misma, el objetivo de una llamada desde bytecode.
bool tipo_abi_soportado(const Type& t) {
    return tipo_soportado(t) && t.kind() != Type::Kind::String;
}

std::string tipo_cpp(const Type& t) {
    switch (t.kind()) {
        case Type::Kind::Int:    return "int64_t";
        case Type::Kind::Float:  return "double";
        case Type::Kind::Bool:   return "bool";
        case Type::Kind::Void:   return "void";
        case Type::Kind::String: return "std::string";
        default: return ""; // inalcanzable si tipo_soportado() dio el visto bueno
    }
}

// Escapa una cadena Lumen para que quepa, literal, en un fichero .cpp.
std::string literal_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += "\"";
    return out;
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

// Los 6 metodos de string que reconoce metodos_de()/call_method()
// (natives.cpp) -- misma lista, vista desde este lado. Cada uno tiene su
// funcion equivalente en string_runtime_prelude(), con la misma semantica
// exacta, y un tipo de retorno FIJO (nunca ambiguo): bool para los tres
// primeros, string para los otros tres.
bool metodo_string_soportado(const std::string& nombre) {
    return nombre == "starts_with" || nombre == "ends_with" || nombre == "contains" ||
           nombre == "upper" || nombre == "lower" || nombre == "trim";
}

bool metodo_string_devuelve_bool(const std::string& nombre) {
    return nombre == "starts_with" || nombre == "ends_with" || nombre == "contains";
}

// ── Comprobacion de tipos y compilabilidad ──────────────────────────────────
//
// Lumen Script no comprueba en ningun sitio -- ni el checker (check_expr/
// check_stmt), ni el VM en tiempo de ejecucion -- que una variable, un
// argumento o un valor de retorno mantengan el tipo con el que se
// declararon: es un lenguaje dinamicamente tipado por debajo de la
// anotacion. `int x = 1; x = "otro tipo"` compila y corre sin aviso; una
// funcion declarada `fn int f(): return "hola"` tambien. Y `a / b` entre
// dos `int` da Int si la division es exacta y Float si no -- una decision
// que solo se puede tomar en tiempo de ejecucion.
//
// La primera version de esta fase confiaba en el tipo DECLARADO (el de
// IrExpr::type / IrStmt::decl_type) para elegir la representacion C++, sin
// verificar que fuera a coincidir con el tipo REAL en tiempo de ejecucion.
// El resultado: programas legales, ya validados por el corpus, que --native
// compilaba sin error y ejecutaba dando una respuesta HTTP DISTINTA a la
// del bytecode -- una violacion directa del invariante de la seccion 3
// ("mismo fuente, mismo comportamiento"), descubierta manualmente al probar
// una reasignacion de tipo y un `and`/`or` con operandos no booleanos.
//
// tipo_provable() reemplaza esa confianza ciega: para cada forma de
// expresion, aplica LAS MISMAS reglas dinamicas que usa el VM (vm.cpp) para
// decidir si el tipo del resultado esta garantizado, y con cual. Cuando no
// puede demostrarlo -- una division entre dos int, un `and`/`or` con
// operandos no booleanos, una variable cuyo tipo no se pudo demostrar mas
// arriba -- devuelve nullopt, y la expresion (o la funcion entera) se queda
// sin compilar a nativo. No es una traduccion de "es compilable" a
// "tipo_de() en emitter.cpp": tipo_de() es deliberadamente debil (Unknown
// para casi todo) porque solo necesita servir a un puñado de comprobaciones
// puntuales del checker; esta funcion necesita ser SOLIDA, asi que es un
// analisis propio, mas estricto y mas completo, solo para esta fase.
class Comprobador {
public:
    explicit Comprobador(const std::vector<std::string>& nombre_por_indice,
                        const TablaFirmas& firmas)
        : nombre_por_indice_(nombre_por_indice), firmas_(firmas) {}

    // Ranura -> tipo declarado, en el orden en que VarDecl/parametros los
    // van presentando -- igual que Generador::ranura_a_nombre_, pero de
    // tipo en vez de nombre. Una vez registrada, una ranura mantiene ESE
    // tipo durante toda la funcion: es la CONSECUENCIA (no la causa) de que
    // stmt_compilable() exija que cualquier Assign(Local) sobre esa ranura
    // demuestre el mismo tipo -- por induccion, un Ident que resuelve aqui
    // tiene garantizado que su valor real coincide siempre.
    void registrar(int slot, Type::Kind k) { ranura_tipos_[slot] = k; }

    // Nullopt si no se puede demostrar; si no, el Type::Kind exacto que el
    // VM SIEMPRE produciria para esta expresion, con los mismos valores.
    std::optional<Type::Kind> tipo_provable(const IrExpr& e) const {
        switch (e.kind) {
            case IrExprKind::IntLit:    return Type::Kind::Int;
            case IrExprKind::FloatLit:  return Type::Kind::Float;
            case IrExprKind::BoolLit:   return Type::Kind::Bool;
            case IrExprKind::StringLit: return Type::Kind::String;

            // Sin representacion en esta fase, o sin sentido fuera de una
            // ruta/clase.
            case IrExprKind::NullLit:
            case IrExprKind::ListLit:
            case IrExprKind::DictLit:
            case IrExprKind::Member:
            case IrExprKind::Index:
            case IrExprKind::Await:
            case IrExprKind::This:
                return std::nullopt;

            case IrExprKind::Ident: {
                auto it = ranura_tipos_.find(e.slot);
                return it == ranura_tipos_.end() ? std::nullopt
                                                 : std::optional<Type::Kind>(it->second);
            }

            case IrExprKind::Unary: {
                if (!e.lhs) return std::nullopt;
                auto t = tipo_provable(*e.lhs);
                if (!t) return std::nullopt;
                if (e.text == "not") return Type::Kind::Bool; // Op::Not: siempre bool
                return es_numerico(*t) ? t : std::nullopt;    // '-': solo sobre numeros
            }

            case IrExprKind::Binary: {
                if (!e.lhs || !e.rhs) return std::nullopt;
                auto tl = tipo_provable(*e.lhs);
                auto tr = tipo_provable(*e.rhs);
                if (!tl || !tr) return std::nullopt;

                // and/or (vm.cpp: JumpIfFalsePeek/JumpIfTruePeek) devuelven
                // el VALOR del operando que gana, al estilo Python -- NO un
                // booleano forzado. Traducirlo a &&/|| (lo que hace
                // Generador::expr) solo coincide, observablemente, cuando
                // los dos lados YA son bool: alli "el operando que gana" y
                // "el resultado de &&/||" son el mismo valor. Para
                // cualquier otro tipo (`5 and 10` -> 10, no `true`) no
                // coinciden, y esta fase no genera la logica de verdad
                // (evaluar una vez, devolver el operando) -- se queda sin
                // compilar.
                if (e.text == "and" || e.text == "or")
                    return (*tl == Type::Kind::Bool && *tr == Type::Kind::Bool)
                               ? std::optional<Type::Kind>(Type::Kind::Bool) : std::nullopt;

                if (e.text == "==" || e.text == "!=" || e.text == "<" || e.text == "<=" ||
                    e.text == ">" || e.text == ">=") {
                    bool numericos = es_numerico(*tl) && es_numerico(*tr);
                    bool strings   = *tl == Type::Kind::String && *tr == Type::Kind::String;
                    return (numericos || strings) ? std::optional<Type::Kind>(Type::Kind::Bool)
                                                  : std::nullopt;
                }

                if (e.text == "+" && *tl == Type::Kind::String && *tr == Type::Kind::String)
                    return Type::Kind::String;

                if (!es_numerico(*tl) || !es_numerico(*tr)) return std::nullopt;

                if (e.text == "%") // vm.cpp: '%' exige enteros a los dos lados
                    return (*tl == Type::Kind::Int && *tr == Type::Kind::Int)
                               ? std::optional<Type::Kind>(Type::Kind::Int) : std::nullopt;

                if (e.text == "/")
                    // vm.cpp: entre dos int, Int si la division es EXACTA y
                    // Float si no -- una rama que solo el valor en tiempo de
                    // ejecucion decide. No demostrable estaticamente.
                    return (*tl == Type::Kind::Int && *tr == Type::Kind::Int)
                               ? std::nullopt : std::optional<Type::Kind>(Type::Kind::Float);

                // +, -, *: Int si los dos son Int, Float en cualquier otra
                // combinacion numerica (vm.cpp: `ints ? integer : real`).
                return (*tl == Type::Kind::Int && *tr == Type::Kind::Int)
                           ? std::optional<Type::Kind>(Type::Kind::Int)
                           : std::optional<Type::Kind>(Type::Kind::Float);
            }

            case IrExprKind::Ternary: {
                // La condicion solo necesita ser demostrable en algun tipo
                // (Int/Float/Bool convierten a bool en C++ identico a
                // truthy(); string no convierte -- g++ lo rechaza solo).
                if (!e.object || !tipo_provable(*e.object)) return std::nullopt;
                if (!e.lhs || !e.rhs) return std::nullopt;
                auto ts = tipo_provable(*e.lhs);
                auto tn = tipo_provable(*e.rhs);
                if (ts && tn && *ts == *tn) return ts;
                return std::nullopt;
            }

            case IrExprKind::PreStep:
            case IrExprKind::PostStep: {
                if (!e.lhs || e.lhs->kind != IrExprKind::Ident) return std::nullopt;
                auto t = tipo_provable(*e.lhs);
                return (t && es_numerico(*t)) ? t : std::nullopt;
            }

            case IrExprKind::Call: {
                if (e.call_shape == IrCallShape::BuiltinMethodCall) {
                    if (!e.object || e.object->type.kind() != Type::Kind::String ||
                        !metodo_string_soportado(e.call_name) || !tipo_provable(*e.object))
                        return std::nullopt;
                    for (const auto& a : e.args)
                        if (!a.value || !tipo_provable(*a.value)) return std::nullopt;
                    return metodo_string_devuelve_bool(e.call_name)
                               ? Type::Kind::Bool : Type::Kind::String;
                }
                if (e.call_shape == IrCallShape::UserFunctionCall) {
                    if (e.call_index < 0 ||
                        static_cast<size_t>(e.call_index) >= nombre_por_indice_.size())
                        return std::nullopt;
                    auto fit = firmas_.find(nombre_por_indice_[static_cast<size_t>(e.call_index)]);
                    if (fit == firmas_.end() || fit->second.params.size() != e.args.size())
                        return std::nullopt;
                    for (size_t i = 0; i < e.args.size(); ++i) {
                        if (!e.args[i].value) return std::nullopt;
                        auto ta = tipo_provable(*e.args[i].value);
                        if (!ta || *ta != fit->second.params[i]) return std::nullopt;
                    }
                    return fit->second.retorno;
                }
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    bool block_compilable(const IrBlock& b, Type::Kind retorno_fn) {
        for (const auto& s : b)
            if (!s || !stmt_compilable(*s, retorno_fn)) return false;
        return true;
    }

    bool stmt_compilable(const IrStmt& s, Type::Kind retorno_fn) {
        switch (s.kind) {
            case IrStmtKind::Return: {
                if (!s.value) return retorno_fn == Type::Kind::Void;
                auto t = tipo_provable(*s.value);
                return t && *t == retorno_fn;
            }

            case IrStmtKind::ExprStmt:
                return s.value && tipo_provable(*s.value).has_value();

            case IrStmtKind::VarDecl: {
                if (!tipo_soportado(s.decl_type)) return false;
                if (s.value) {
                    auto t = tipo_provable(*s.value);
                    if (!t || *t != s.decl_type.kind()) return false;
                }
                // Registrar DESPUES de comprobar el valor: una redeclaracion
                // (mismo nombre, ranura nueva) no debe validarse contra si
                // misma.
                registrar(s.slot, s.decl_type.kind());
                return true;
            }

            // Solo la forma Local: Session/Index/Member implican sesion, un
            // contenedor o una clase. El nuevo valor tiene que demostrar
            // EXACTAMENTE el tipo con el que esa ranura se declaro -- es la
            // regla que mantiene solido tipo_provable(Ident) durante el
            // resto de la funcion (ver el comentario de registrar()).
            case IrStmtKind::Assign: {
                if (s.assign_target != IrAssignTarget::Local || !s.value) return false;
                auto original = ranura_tipos_.find(s.assign_slot);
                if (original == ranura_tipos_.end()) return false;
                auto t = tipo_provable(*s.value);
                return t && *t == original->second;
            }

            case IrStmtKind::If:
                return s.value && tipo_provable(*s.value).has_value() &&
                       block_compilable(s.body, retorno_fn) &&
                       block_compilable(s.orelse, retorno_fn);

            case IrStmtKind::While:
                return s.value && tipo_provable(*s.value).has_value() &&
                       block_compilable(s.body, retorno_fn);

            case IrStmtKind::Break:
            case IrStmtKind::Continue:
                return true;

            // For itera una List (contenedor). Require y Try no son
            // "primitivos y control de flujo" en el sentido estrecho de
            // esta fase todavia -- Try en concreto necesita decidir como se
            // representa un error nativo, que es una decision de la fase 5
            // (asincronia y errores).
            case IrStmtKind::For:
            case IrStmtKind::Require:
            case IrStmtKind::Try:
                return false;
        }
        return false;
    }

private:
    const std::vector<std::string>& nombre_por_indice_;
    const TablaFirmas&               firmas_;
    std::map<int, Type::Kind>        ranura_tipos_;
};

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
        {"+", "+"}, {"-", "-"}, {"*", "*"},
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
            case IrExprKind::IntLit:    return std::to_string(e.int_value) + "LL";
            case IrExprKind::FloatLit:  return literal_float(e.float_value);
            case IrExprKind::BoolLit:   return e.bool_value ? "true" : "false";
            case IrExprKind::StringLit: return "std::string(" + literal_string(e.text) + ")";
            case IrExprKind::Ident:     return nombre_cpp(e.text);

            case IrExprKind::Unary:
                return std::string("(") + (e.text == "not" ? "!" : "-") + expr(*e.lhs) + ")";

            // '/' y '%' con un ayudante que comprueba el divisor antes de
            // dividir (ver error_runtime_prelude): el resto de operadores
            // no tiene ningun caso de fallo en tiempo de ejecucion que el
            // VM trate como error controlado, asi que van directos al
            // operador de C++ equivalente.
            case IrExprKind::Binary:
                if (e.text == "/")
                    return "lumen_div_check(" + expr(*e.lhs) + ", " + expr(*e.rhs) + ")";
                if (e.text == "%")
                    return "lumen_mod_check(" + expr(*e.lhs) + ", " + expr(*e.rhs) + ")";
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
                // BuiltinMethodCall (solo metodos de string, ver
                // metodo_string_soportado): funcion libre de
                // string_runtime_prelude(), receptor primero, luego los
                // argumentos -- mismo orden que call_method(recv, args) en
                // natives.cpp, solo que en tiempo de compilacion en vez de
                // por nombre en tiempo de ejecucion.
                std::string s = e.call_shape == IrCallShape::BuiltinMethodCall
                                   ? "lumen_str_" + e.call_name
                                   : nombre_cpp(nombre_por_indice_.at(static_cast<size_t>(e.call_index)));
                s += "(";
                if (e.call_shape == IrCallShape::BuiltinMethodCall) s += expr(*e.object);
                for (size_t i = 0; i < e.args.size(); ++i) {
                    if (i || e.call_shape == IrCallShape::BuiltinMethodCall) s += ", ";
                    s += expr(*e.args[i].value);
                }
                s += ")";
                return s;
            }

            default:
                return ""; // inalcanzable: Comprobador ya lo descarto antes de llegar aqui
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
                return ""; // inalcanzable: Comprobador ya lo descarto antes de llegar aqui
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
                                                     const std::vector<std::string>& nombre_por_indice,
                                                     const TablaFirmas& firmas) {
    const Type retorno_decl = Type::from_declared(fn.return_type);
    if (!tipo_soportado(retorno_decl)) return std::nullopt;
    for (const auto& p : fn.params)
        if (!tipo_soportado(Type::from_declared(p.type))) return std::nullopt;

    Comprobador comprobador(nombre_por_indice, firmas);
    // check_function declara los parametros, en orden, antes que nada mas
    // (ver Emitter::check_function): la ranura i-esima es siempre el
    // parametro i-esimo -- mismo orden que Generador::registrar() mas abajo.
    for (size_t i = 0; i < fn.params.size(); ++i)
        comprobador.registrar(static_cast<int>(i),
                              Type::from_declared(fn.params[i].type).kind());
    if (!comprobador.block_compilable(body, retorno_decl.kind())) return std::nullopt;

    FuncionNativa out;
    out.nombre_lumen = fn.name;

    std::string params;
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (i) params += ", ";
        params += tipo_cpp(Type::from_declared(fn.params[i].type)) + " " +
                  nombre_cpp(fn.params[i].name);
    }
    out.firma_cpp = tipo_cpp(retorno_decl) + " " + nombre_cpp(fn.name) + "(" + params + ")";

    Generador gen(nombre_por_indice);
    for (size_t i = 0; i < fn.params.size(); ++i)
        gen.registrar(static_cast<int>(i), fn.params[i].name);
    out.cuerpo_cpp = "{\n" + gen.block(body, 1) + "}";

    // El wrapper de ABI fija (ver native_abi.hpp): descomprime args[i] al
    // tipo real de cada parametro, llama a la funcion de arriba, y empaqueta
    // el resultado -- o, si la funcion es void, un NativeValue::Tag::Int a 0
    // que nadie mira (el llamante conoce el tipo de retorno declarado, igual
    // que ya conoce la aridad, asi que un valor void nunca se desempaqueta).
    // El try/catch alrededor de todo es el otro lado del canal de error
    // (ver error_runtime_prelude): una funcion nativa que se ejecuta hasta
    // el final sin invocar lumen_native_fail() nunca lo atraviesa, asi que
    // no cuesta nada en el camino normal.
    //
    // Si la frontera de la funcion usa `string`, la ABI fija de hoy no la
    // representa (ver tipo_abi_soportado): el cuerpo de arriba se genera
    // igual -- otra funcion nativa que la llame directamente se beneficia --
    // pero sin wrapper, se queda fuera del despacho desde la VM.
    bool frontera_cruza_abi = tipo_abi_soportado(retorno_decl);
    for (const auto& p : fn.params)
        frontera_cruza_abi = frontera_cruza_abi && tipo_abi_soportado(Type::from_declared(p.type));
    if (!frontera_cruza_abi) return out;

    out.simbolo_abi = "lumen_native_" + fn.name;
    std::string cuerpo_wrapper = "    (void)argc;\n    try {\n";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        const Type t = Type::from_declared(fn.params[i].type);
        cuerpo_wrapper += "        " + tipo_cpp(t) + " " + nombre_cpp(fn.params[i].name) +
                          " = args[" + std::to_string(i) + "]." + campo_abi(t.kind()) + ";\n";
    }
    const std::string llamada = nombre_cpp(fn.name) + "(" + [&] {
        std::string s;
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (i) s += ", ";
            s += nombre_cpp(fn.params[i].name);
        }
        return s;
    }() + ")";
    if (retorno_decl.kind() == Type::Kind::Void) {
        cuerpo_wrapper += "        " + llamada + ";\n"
                          "        NativeValue salida; salida.tag = NativeValue::Tag::Int; "
                          "salida.i = 0;\n"
                          "        return salida;\n";
    } else {
        cuerpo_wrapper += "        NativeValue salida; salida.tag = " +
                          etiqueta_abi(retorno_decl.kind()) + "; salida." +
                          campo_abi(retorno_decl.kind()) + " = " + llamada + ";\n"
                          "        return salida;\n";
    }
    cuerpo_wrapper += "    } catch (const LumenNativeError&) {\n"
                      "        NativeValue error; error.tag = NativeValue::Tag::Error; "
                      "error.i = 0;\n"
                      "        return error;\n"
                      "    }\n}";
    out.wrapper_cpp = "extern \"C\" NativeValue " + out.simbolo_abi +
                      "(const NativeValue* args, int32_t argc) {\n" +
                      cuerpo_wrapper;
    return out;
}

std::string abi_prelude() {
    // Identico, campo a campo, a la definicion de include/lumen_script/native_abi.hpp.
    return
        "struct NativeValue {\n"
        "    enum class Tag : int32_t { Int, Float, Bool, Error } tag = Tag::Int;\n"
        "    union { int64_t i; double d; bool b; };\n"
        "};\n"
        "using CompiledFn = NativeValue (*)(const NativeValue*, int32_t);\n";
}

std::string string_runtime_prelude() {
    // Cada una calca, a proposito, la rama `string` de call_method() en
    // natives.cpp -- misma logica, tipos nativos en vez de Value.
    return
        "static bool lumen_str_starts_with(const std::string& s, const std::string& n) {\n"
        "    return s.rfind(n, 0) == 0;\n"
        "}\n"
        "static bool lumen_str_ends_with(const std::string& s, const std::string& n) {\n"
        "    return s.size() >= n.size() && s.compare(s.size() - n.size(), n.size(), n) == 0;\n"
        "}\n"
        "static bool lumen_str_contains(const std::string& s, const std::string& n) {\n"
        "    return s.find(n) != std::string::npos;\n"
        "}\n"
        "static std::string lumen_str_upper(std::string s) {\n"
        "    for (char& c : s) c = static_cast<char>(::toupper((unsigned char)c));\n"
        "    return s;\n"
        "}\n"
        "static std::string lumen_str_lower(std::string s) {\n"
        "    for (char& c : s) c = static_cast<char>(::tolower((unsigned char)c));\n"
        "    return s;\n"
        "}\n"
        "static std::string lumen_str_trim(const std::string& s) {\n"
        "    size_t a = s.find_first_not_of(\" \\t\\r\\n\");\n"
        "    if (a == std::string::npos) return \"\";\n"
        "    size_t b = s.find_last_not_of(\" \\t\\r\\n\");\n"
        "    return s.substr(a, b - a + 1);\n"
        "}\n";
}

std::string error_runtime_prelude() {
    // g_lumen_native_error: por hilo, porque varias peticiones concurrentes
    // pueden estar cada una a mitad de una llamada nativa a la vez. El
    // mensaje es valido hasta la SIGUIENTE llamada nativa en el mismo hilo
    // -- quien lo lee (vm.cpp) lo copia a su propio std::string antes de
    // hacer cualquier otra cosa.
    //
    // lumen_div_check/lumen_mod_check son los unicos puntos de fallo que
    // introduce esta fase (division y modulo por cero, vm.cpp: "division
    // por cero" / "modulo por cero") -- plantillas porque el generador los
    // usa tanto para int64_t/int64_t (modulo) como para cualquier mezcla de
    // int64_t/double (division; ver tipo_provable(), que ya garantiza que
    // una division entre dos int nunca llega aqui).
    return
        "static thread_local std::string g_lumen_native_error;\n"
        "struct LumenNativeError {};\n"
        "[[noreturn]] static void lumen_native_fail(std::string msg) {\n"
        "    g_lumen_native_error = std::move(msg);\n"
        "    throw LumenNativeError{};\n"
        "}\n"
        "extern \"C\" const char* lumen_native_error_message() {\n"
        "    return g_lumen_native_error.c_str();\n"
        "}\n"
        "template <class T, class U>\n"
        "static auto lumen_div_check(T a, U b) {\n"
        "    if (b == 0) lumen_native_fail(\"division por cero\");\n"
        "    return a / b;\n"
        "}\n"
        "template <class T, class U>\n"
        "static auto lumen_mod_check(T a, U b) {\n"
        "    if (b == 0) lumen_native_fail(\"modulo por cero\");\n"
        "    return a % b;\n"
        "}\n";
}

} // namespace lumen_script
