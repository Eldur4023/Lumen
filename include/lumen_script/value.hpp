#pragma once
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>


namespace lumen_script {

// Valor en tiempo de ejecucion del VM.
//
// Ocupa 16 bytes: 8 de carga y 1 de etiqueta.  Un valor es exactamente de un
// tipo a la vez, asi que el puntero al monton comparte hueco con los escalares
// en una union.  Antes eran 72 —tres shared_ptr, dos de ellos siempre nulos—, y
// cada push y cada pop del VM movia esos 72 bytes.
class Value {
public:
    enum class Type { Null, Bool, Int, Float, Str, List, Dict };

    using List = std::vector<Value>;

    // Diccionario del VM.
    //
    // Era un std::map: un arbol rojo-negro, con una asignacion por nodo y un
    // salto de puntero por acceso, para objetos que casi siempre tienen entre 3
    // y 10 claves.  Medido con claves de cadena, el recorrido lineal sobre un
    // vector contiguo va el doble de rapido por debajo de 8 claves y el arbol
    // no gana hasta las 64.
    //
    // Asi que aqui va siempre un vector, en orden de insercion, y al pasar de
    // un umbral se le anade un indice hash.  De ese modo la busqueda tambien es
    // O(1) en los diccionarios grandes —un acumulador, un contador de
    // palabras—, donde el recorrido lineal seria un desastre, y la insercion es
    // O(1) amortizada en todos los tamanos, cosa que el arbol nunca dio.
    //
    // El orden de las claves de la respuesta no cambia: lo fija nlohmann al
    // construir el objeto JSON, no este contenedor.
    class Dict {
    public:
        using Par            = std::pair<std::string, Value>;
        using iterator       = std::vector<Par>::iterator;
        using const_iterator = std::vector<Par>::const_iterator;

        iterator       begin()       { return v_.begin(); }
        iterator       end()         { return v_.end();   }
        const_iterator begin() const { return v_.begin(); }
        const_iterator end()   const { return v_.end();   }

        size_t size()  const { return v_.size();  }
        bool   empty() const { return v_.empty(); }
        void   clear()       { v_.clear(); idx_.clear(); }

        // Quien sabe cuantas claves va a meter —el MakeDict del VM las tiene
        // contadas— evita que el vector crezca a saltos: sin esto un
        // diccionario de 3 claves hace tres asignaciones, una por realojo.
        void   reservar(size_t n) { v_.reserve(n); }
        size_t count(std::string_view k) const { return find(k) == end() ? 0 : 1; }

        iterator       find(std::string_view k);
        const_iterator find(std::string_view k) const;
        Value&         operator[](std::string_view k);

    private:
        // Busqueda heterogenea: sin esto, cada find con un const char* o un
        // string_view construiria un std::string solo para consultar.
        struct Hash {
            using is_transparent = void;
            size_t operator()(std::string_view s) const noexcept {
                return std::hash<std::string_view>{}(s);
            }
        };
        struct Igual {
            using is_transparent = void;
            bool operator()(std::string_view a, std::string_view b) const noexcept {
                return a == b;
            }
        };

        // Por debajo del umbral no hay indice: mantenerlo costaria mas que
        // recorrer el vector entero.
        static constexpr size_t kUmbral = 16;

        void indexar();

        std::vector<Par>                                    v_;
        std::unordered_map<std::string, size_t, Hash, Igual> idx_;
    };

    Value() = default;
    ~Value() { soltar(); }

    Value(const Value& o) : type_(o.type_) { carga_de(o); retener(); }
    Value(Value&& o) noexcept : type_(o.type_) { carga_de(o); o.type_ = Type::Null; }

    Value& operator=(const Value& o) {
        if (this != &o) {
            o.retener();            // antes de soltar: puede ser la misma caja
            soltar();
            type_ = o.type_;
            carga_de(o);
        }
        return *this;
    }
    Value& operator=(Value&& o) noexcept {
        if (this != &o) {
            soltar();
            type_ = o.type_;
            carga_de(o);
            o.type_ = Type::Null;
        }
        return *this;
    }

    static Value null()               { return Value(); }
    static Value boolean(bool b)      { Value v; v.type_ = Type::Bool;  v.b_ = b; return v; }
    static Value integer(long long i) { Value v; v.type_ = Type::Int;   v.i_ = i; return v; }
    static Value real(double d)       { Value v; v.type_ = Type::Float; v.d_ = d; return v; }

    static Value str(std::string s) {
        Value v; v.type_ = Type::Str;  v.o_ = new CStr(std::move(s));  return v;
    }
    static Value list(List l = {}) {
        Value v; v.type_ = Type::List; v.o_ = new CList(std::move(l)); return v;
    }
    static Value dict(Dict d = {}) {
        Value v; v.type_ = Type::Dict; v.o_ = new CDict(std::move(d)); return v;
    }

    Type type() const { return type_; }

    bool is_null()  const { return type_ == Type::Null; }
    bool is_bool()  const { return type_ == Type::Bool; }
    bool is_int()   const { return type_ == Type::Int; }
    bool is_float() const { return type_ == Type::Float; }
    bool is_num()   const { return is_int() || is_float(); }
    bool is_str()   const { return type_ == Type::Str; }
    bool is_list()  const { return type_ == Type::List; }
    bool is_dict()  const { return type_ == Type::Dict; }

    bool               as_bool()  const { return b_; }
    long long          as_int()   const { return i_; }
    double             as_float() const { return is_float() ? d_ : double(i_); }
    const std::string& as_str()   const { return static_cast<CStr*>(o_)->v;  }
    List&              as_list()  const { return static_cast<CList*>(o_)->v; }
    Dict&              as_dict()  const { return static_cast<CDict*>(o_)->v; }

    // Verdad de Lumen Script, al estilo Python: son falsos null, false, 0, 0.0, la
    // cadena vacia y los contenedores vacios.
    //
    // Esto NO es la coercion de JavaScript que critica el README: aquello es
    // convertir tipos dentro de '==' a espaldas de quien escribe. Aqui no hay
    // conversion, solo una lectura en contexto booleano — la misma que hacen
    // Python y C++, los dos idiomas de los que sale Lumen Script.
    bool truthy() const {
        switch (type_) {
            case Type::Null:  return false;
            case Type::Bool:  return b_;
            case Type::Int:   return i_ != 0;
            case Type::Float: return d_ != 0;
            case Type::Str:   return !as_str().empty();
            case Type::List:  return !as_list().empty();
            case Type::Dict:  return !as_dict().empty();
        }
        return false;
    }

    const char* type_name() const;
    std::string to_string() const;          // representacion para text()/concatenacion
    // Serializacion: escribe el JSON directamente en el bufer, de una pasada.
    void        write_json(std::string& out) const;
    std::string to_json_text() const { std::string s; write_json(s); return s; }

    // Parsea JSON a un Value, sin arbol intermedio.  Devuelve false ante
    // cualquier cosa que no sea JSON valido y completo, basura al final
    // incluida.  Ver src/lumen_script/json_parse.cpp.
    static bool parse_json(std::string_view texto, Value& out);

    bool equals(const Value& o) const;

private:
    // Caja con contador propio.  Atomico porque un valor puede nacer en un hilo
    // del pool de base de datos y consumirse en el del event loop: el traspaso
    // esta sincronizado, pero la caja puede quedar compartida entre los dos.
    struct Caja { std::atomic<unsigned> rc{1}; };
    template <typename T>
    struct CajaDe : Caja { T v; explicit CajaDe(T x) : v(std::move(x)) {} };

    using CStr  = CajaDe<std::string>;
    using CList = CajaDe<List>;
    using CDict = CajaDe<Dict>;

    bool en_monton() const {
        return type_ == Type::Str || type_ == Type::List || type_ == Type::Dict;
    }
    void carga_de(const Value& o) { std::memcpy(&i_, &o.i_, sizeof(i_)); }
    void retener() const {
        if (en_monton()) o_->rc.fetch_add(1, std::memory_order_relaxed);
    }
    void soltar() {
        if (!en_monton()) return;
        if (o_->rc.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        switch (type_) {
            case Type::Str:  delete static_cast<CStr*>(o_);  break;
            case Type::List: delete static_cast<CList*>(o_); break;
            case Type::Dict: delete static_cast<CDict*>(o_); break;
            default: break;
        }
    }

    union {
        long long i_ = 0;
        bool      b_;
        double    d_;
        Caja*     o_;
    };
    Type type_ = Type::Null;
};

// ─── Value::Dict ─────────────────────────────────────────────────────────────
// Fuera de la clase porque necesitan que Value este completo.

inline void Value::Dict::indexar() {
    idx_.clear();
    idx_.reserve(v_.size() * 2);
    for (size_t i = 0; i < v_.size(); ++i) idx_.emplace(v_[i].first, i);
}

inline Value::Dict::iterator Value::Dict::find(std::string_view k) {
    if (!idx_.empty()) {
        auto it = idx_.find(k);
        return it == idx_.end() ? v_.end()
                                : v_.begin() + static_cast<std::ptrdiff_t>(it->second);
    }
    for (auto it = v_.begin(); it != v_.end(); ++it)
        if (it->first == k) return it;
    return v_.end();
}

inline Value::Dict::const_iterator Value::Dict::find(std::string_view k) const {
    if (!idx_.empty()) {
        auto it = idx_.find(k);
        return it == idx_.end() ? v_.end()
                                : v_.begin() + static_cast<std::ptrdiff_t>(it->second);
    }
    for (auto it = v_.begin(); it != v_.end(); ++it)
        if (it->first == k) return it;
    return v_.end();
}

inline Value& Value::Dict::operator[](std::string_view k) {
    if (auto it = find(k); it != v_.end()) return it->second;

    v_.emplace_back(std::string(k), Value());
    if (!idx_.empty())            idx_.emplace(v_.back().first, v_.size() - 1);
    else if (v_.size() > kUmbral) indexar();
    return v_.back().second;
}

} // namespace lumen_script
