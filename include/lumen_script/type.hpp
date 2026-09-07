#pragma once
#include <memory>
#include <string>

namespace lumen_script {

struct TypeRef; // ast.hpp

// Representacion tipada de un tipo de Lumen Script, pensada para sustituir a
// las cadenas ad-hoc que usa hoy Emitter (Local::type, tipo_de()) y para ser
// la base del IR tipado que necesita el backend de compilacion nativa
// (COMPILACION-NATIVA.md, fase 1).
//
// TODAVIA NO ESTA CONECTADO a Emitter/VM: este fichero es puramente aditivo,
// un primer paso seguro de la fase 1. Emitter sigue usando std::string hasta
// que se haga esa migracion, que es un cambio aparte y revisable por su
// cuenta -- from_declared()/base_name() son el puente para cuando llegue:
// reproducen exactamente lo que hoy produce `TypeRef.name` sin necesitar
// cambiar Local::type todavia.
//
// Decisiones que siguen a la gramatica al pie de la letra (ver
// LUMEN_SCRIPT-GRAMMAR.md):
//   - int/long son EL MISMO TIPO (§7): un solo Kind::Int. Pero el checker
//     actual conserva la ortografia exacta en sus mensajes de error ("los
//     valores de tipo long no tienen..."), asi que Type tambien la conserva
//     (campo spelling_) para no cambiar ni un caracter de un diagnostico
//     existente cuando esto se conecte.
//   - float/double son EL MISMO TIPO (§7): un solo Kind::Float, misma nota.
//   - La clave de un Dict siempre es string (§8): Dict solo lleva el tipo del
//     valor, no un par de tipos.
//   - Los genericos se borran al compilar (§8), pero aqui SI se conservan:
//     el checker que consuma esto necesita saber que hay dentro de un
//     List<T>/Dict<string,V> para decidir la representacion nativa
//     (COMPILACION-NATIVA.md §7), aunque el VM de bytecode los borre luego.

class Type {
public:
    // Unknown es el "no se sabe" de hoy (la cadena vacia que devuelve
    // tipo_de() cuando no hay nada evidente que comprobar: una variable de
    // for sobre un Json, una expresion que no es un literal ni un
    // identificador declarado...). Es un estado real, distinto de Void (una
    // fn que no devuelve nada SI es un tipo conocido).
    enum class Kind { Unknown, Void, Int, Float, Bool, String, List, Dict, Class, Json };

    Kind kind() const { return kind_; }
    bool is_optional() const { return optional_; }
    bool is_unknown() const { return kind_ == Kind::Unknown; }

    // Solo tiene sentido si kind() == List (el tipo de los elementos) o
    // kind() == Dict (el tipo de los valores; la clave siempre es string).
    const Type& element() const { return *elem_; }

    // Solo tiene sentido si kind() == Class.
    const std::string& class_name() const { return class_name_; }

    static Type unknown() { return Type(Kind::Unknown); }
    static Type primitive(Kind k) { return Type(k); }
    static Type list_of(Type elem) { return Type(Kind::List, std::move(elem)); }
    static Type dict_of(Type value) { return Type(Kind::Dict, std::move(value)); }
    static Type class_ref(std::string name) { return Type(Kind::Class, std::move(name)); }
    static Type json() { return Type(Kind::Json); }
    static Type void_() { return Type(Kind::Void); }

    // A partir de un TypeRef del AST (ast.hpp), tal como aparece escrito en
    // el .lum: "int x", "long x", "List<Usuario> xs"... Conserva la
    // ortografia exacta ("long", "double") por la razon de arriba. Ver
    // parser.cpp: TypeRef::str() para la nocion de "tipo declarado" de la
    // que parte esto. Implementado en type.cpp (necesita ast.hpp completo).
    static Type from_declared(const TypeRef& t);

    // A partir de un nombre desnudo ya resuelto (lo que hoy lleva
    // NombreTipado::tipo: un campo de clase, o el tipo exacto de un Value
    // constante). "" da unknown(). No hay generics ni '?' que reconstruir
    // porque NombreTipado nunca los llevo.
    static Type from_legacy_name(const std::string& name);

    // El nombre desnudo tal y como lo usan hoy Local::type/tipo_de: sin `?`,
    // sin los argumentos de un generico ("List<int>" da "List", no
    // "List<int>"). Es la clave de busqueda que ya esperan ClassSigs y
    // metodos_de() (natives.hpp) -- no to_string(), que es mas descriptivo
    // pero no es la clave que usan esas tablas. "" si is_unknown().
    std::string base_name() const;

    // Copia con el sufijo `?` puesto o quitado; el resto del tipo no cambia.
    Type with_optional(bool opt) const {
        Type t = *this;
        t.optional_ = opt;
        return t;
    }

    // La igualdad ignora spelling_ a proposito: `int` y `long` son el MISMO
    // tipo (§7), y es lo que debe decidir si algo type-checkea, no con que
    // palabra se escribio.
    bool operator==(const Type& other) const {
        if (kind_ != other.kind_ || optional_ != other.optional_) return false;
        if (kind_ == Kind::Class) return class_name_ == other.class_name_;
        if (kind_ == Kind::List || kind_ == Kind::Dict) return *elem_ == *other.elem_;
        return true;
    }
    bool operator!=(const Type& other) const { return !(*this == other); }

    // Notacion descriptiva completa: "int", "List<string>",
    // "Dict<string,Json>", "MiClase", "int?"... Para mensajes nuevos (el IR,
    // el backend nativo). NO es la clave de busqueda (ver base_name()) ni
    // reproduce necesariamente un mensaje de error ya existente.
    std::string to_string() const {
        std::string base = spelling_.empty() ? canonical_name() : spelling_;
        if (kind_ == Kind::List) base = "List<" + elem_->to_string() + ">";
        if (kind_ == Kind::Dict) base = "Dict<string," + elem_->to_string() + ">";
        return optional_ ? base + "?" : base;
    }

private:
    explicit Type(Kind k) : kind_(k) {}
    Type(Kind k, Type inner) : kind_(k), elem_(std::make_shared<Type>(std::move(inner))) {}
    Type(Kind k, std::string name) : kind_(k), class_name_(std::move(name)) {}

    std::string canonical_name() const {
        switch (kind_) {
            case Kind::Unknown: return "";
            case Kind::Void:    return "void";
            case Kind::Int:     return "int";
            case Kind::Float:   return "float";
            case Kind::Bool:    return "bool";
            case Kind::String:  return "string";
            case Kind::Json:    return "Json";
            case Kind::Class:   return class_name_;
            case Kind::List:    return "List";
            case Kind::Dict:    return "Dict";
        }
        return "";
    }

    Kind                  kind_;
    bool                  optional_ = false;
    std::shared_ptr<Type> elem_;        // List/Dict
    std::string           class_name_;  // Class
    std::string           spelling_;    // ortografia exacta del .lum, si viene de from_declared
};

} // namespace lumen_script
