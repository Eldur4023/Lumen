#pragma once
#include <memory>
#include <string>

namespace lumen_script {

// Representacion tipada de un tipo de Lumen Script, pensada para sustituir a
// las cadenas ad-hoc que usa hoy Emitter (Local::type, tipo_de()) y para ser
// la base del IR tipado que necesita el backend de compilacion nativa
// (COMPILACION-NATIVA.md, fase 1).
//
// TODAVIA NO ESTA CONECTADO a Emitter/VM: este fichero es puramente aditivo,
// un primer paso seguro de la fase 1. Emitter sigue usando std::string hasta
// que se haga esa migracion, que es un cambio aparte y revisable por su
// cuenta (comparar cadenas contra Type::to_string() hace la transicion
// mecanica cuando llegue el momento, sin tener que cambiar los dos a la vez).
//
// Decisiones que siguen a la gramatica al pie de la letra (ver
// LUMEN_SCRIPT-GRAMMAR.md):
//   - int/long son EL MISMO TIPO (§7): un solo Kind::Int.
//   - float/double son EL MISMO TIPO (§7): un solo Kind::Float.
//   - La clave de un Dict siempre es string (§8): Dict solo lleva el tipo del
//     valor, no un par de tipos.
//   - Los genericos se borran al compilar (§8), pero aqui SI se conservan:
//     el checker que consuma esto necesita saber que hay dentro de un
//     List<T>/Dict<string,V> para decidir la representacion nativa
//     (COMPILACION-NATIVA.md §7), aunque el VM de bytecode los borre luego.

class Type {
public:
    enum class Kind { Void, Int, Float, Bool, String, List, Dict, Class, Json };

    Kind kind() const { return kind_; }
    bool is_optional() const { return optional_; }

    // Solo tiene sentido si kind() == List (el tipo de los elementos) o
    // kind() == Dict (el tipo de los valores; la clave siempre es string).
    const Type& element() const { return *elem_; }

    // Solo tiene sentido si kind() == Class.
    const std::string& class_name() const { return class_name_; }

    static Type primitive(Kind k) { return Type(k); }
    static Type list_of(Type elem) { return Type(Kind::List, std::move(elem)); }
    static Type dict_of(Type value) { return Type(Kind::Dict, std::move(value)); }
    static Type class_ref(std::string name) { return Type(Kind::Class, std::move(name)); }
    static Type json() { return Type(Kind::Json); }
    static Type void_() { return Type(Kind::Void); }

    // Copia con el sufijo `?` puesto o quitado; el resto del tipo no cambia.
    Type with_optional(bool opt) const {
        Type t = *this;
        t.optional_ = opt;
        return t;
    }

    bool operator==(const Type& other) const {
        if (kind_ != other.kind_ || optional_ != other.optional_) return false;
        if (kind_ == Kind::Class) return class_name_ == other.class_name_;
        if (kind_ == Kind::List || kind_ == Kind::Dict) return *elem_ == *other.elem_;
        return true;
    }
    bool operator!=(const Type& other) const { return !(*this == other); }

    // Misma notacion que usa hoy el checker en sus mensajes de error: "int",
    // "List<string>", "Dict<string,Json>", "MiClase", "int?"...
    std::string to_string() const {
        std::string base;
        switch (kind_) {
            case Kind::Void:   base = "void"; break;
            case Kind::Int:    base = "int"; break;
            case Kind::Float:  base = "float"; break;
            case Kind::Bool:   base = "bool"; break;
            case Kind::String: base = "string"; break;
            case Kind::Json:   base = "Json"; break;
            case Kind::Class:  base = class_name_; break;
            case Kind::List:   base = "List<" + elem_->to_string() + ">"; break;
            case Kind::Dict:   base = "Dict<string," + elem_->to_string() + ">"; break;
        }
        return optional_ ? base + "?" : base;
    }

private:
    explicit Type(Kind k) : kind_(k) {}
    Type(Kind k, Type inner) : kind_(k), elem_(std::make_shared<Type>(std::move(inner))) {}
    Type(Kind k, std::string name) : kind_(k), class_name_(std::move(name)) {}

    Kind                  kind_;
    bool                  optional_ = false;
    std::shared_ptr<Type> elem_;        // List/Dict
    std::string           class_name_;  // Class
};

} // namespace lumen_script
