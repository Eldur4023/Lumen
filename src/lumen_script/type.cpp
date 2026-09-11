#include <lumen_script/type.hpp>
#include <lumen_script/ast.hpp>

namespace lumen_script {

Type Type::from_declared(const TypeRef& t) {
    Type result = Type::unknown();

    if (t.name == "int" || t.name == "long") {
        result = Type::primitive(Kind::Int);
        result.spelling_ = t.name;
    } else if (t.name == "float" || t.name == "double") {
        result = Type::primitive(Kind::Float);
        result.spelling_ = t.name;
    } else if (t.name == "bool") {
        result = Type::primitive(Kind::Bool);
    } else if (t.name == "string") {
        result = Type::primitive(Kind::String);
    } else if (t.name == "void") {
        result = Type::void_();
    } else if (t.name == "Json") {
        result = Type::json();
    } else if (t.name == "List") {
        // El elemento no lo usa hoy ningun sitio (Local::type solo guarda
        // "List"), pero se conserva para cuando el IR de la fase 1 lo
        // necesite de verdad -- Json como placeholder de "no se especifico".
        Type elem = t.args.empty() ? Type::json() : Type::from_declared(t.args[0]);
        result = Type::list_of(std::move(elem));
    } else if (t.name == "Dict") {
        // args[0] es la clave (siempre string, ver §8 de la gramatica: no se
        // conserva, dict_of() solo lleva el tipo del valor). args[1] es el
        // valor.
        Type value = t.args.size() >= 2 ? Type::from_declared(t.args[1]) : Type::json();
        result = Type::dict_of(std::move(value));
    } else {
        result = Type::class_ref(t.name);
    }

    result.optional_ = t.optional;
    return result;
}

Type Type::from_legacy_name(const std::string& name) {
    if (name.empty()) return Type::unknown();
    TypeRef t;
    t.name = name;
    return from_declared(t);
}

std::string Type::base_name() const {
    return spelling_.empty() ? canonical_name() : spelling_;
}

} // namespace lumen_script
