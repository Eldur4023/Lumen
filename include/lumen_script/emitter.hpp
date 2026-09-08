#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast.hpp"
#include "bytecode.hpp"
#include "diagnostic.hpp"
#include "ir.hpp"
#include "type.hpp"

namespace lumen_script {

// Traduce el cuerpo de una ruta a bytecode.
//
// Resuelve los nombres a ranuras locales en tiempo de compilacion, asi que el
// VM nunca busca una variable por nombre: LoadLocal es un indice directo.
// Lo que el emisor necesita saber de una funcion de usuario para poder
// llamarla: donde esta y que argumentos admite.
struct FnSig {
    size_t index    = 0;
    size_t required = 0;                    // parametros sin valor por defecto
    std::vector<const Expr*> defaults;      // uno por parametro; nulo si no tiene
};
using FunctionSigs = std::map<std::string, FnSig>;

// Lo que el emisor necesita saber de una clase para construirla y llamar a sus
// metodos.  Ambos se compilan a funciones con `this` como primer parametro.
struct ClassSig {
    std::map<std::string, FnSig> methods;
    std::map<size_t, size_t>     ctors;    // numero de parametros -> indice
    std::vector<std::string>     fields;
};
using ClassSigs = std::map<std::string, ClassSig>;

struct Plantilla;

// Un nombre visible dentro de una expresion suelta, con su tipo declarado.
//
// El tipo puede ir vacio y entonces no se comprueba nada sobre el: es lo que
// pasa con la variable de un {% for %}, cuyo tipo depende de lo que lleve
// dentro la lista.
struct NombreTipado {
    std::string nombre;
    std::string tipo;

    // Sin tipo es el caso normal, asi que se escribe solo el nombre.  El
    // literal necesita su propio constructor: de const char* a NombreTipado
    // hay dos conversiones y el compilador solo hace una.
    NombreTipado(std::string n, std::string t = {})
        : nombre(std::move(n)), tipo(std::move(t)) {}
    NombreTipado(const char* n) : nombre(n) {}
};

// Donde estan las plantillas y donde se guardan ya compiladas.
//
// Cuando el emisor ve un render("x.html", k=v) con el nombre literal, compila
// la plantilla AHI MISMO contra esas claves.  Por eso una errata dentro de un
// {{ }} sale en `lumen --check` y no cuando alguien pide la pagina.
struct PlantillaCtx {
    std::string             dir;
    std::vector<Plantilla>* tabla = nullptr;
};

class Emitter {
public:
    // `functions` mapea nombre de funcion de usuario a su indice en la tabla
    // del modulo.  Se resuelve al emitir, asi que el VM no busca por nombre.
    explicit Emitter(DiagnosticBag& diags, const FunctionSigs* functions = nullptr,
                     const ClassSigs* classes = nullptr,
                     const std::set<std::string>* imports = nullptr,
                     PlantillaCtx* plantillas = nullptr)
        : diags_(diags), functions_(functions), classes_(classes), imports_(imports),
          plantillas_(plantillas) {}

    // Los parametros de la ruta ocupan las primeras ranuras, en orden.
    // Devuelve false si algo del cuerpo no se puede compilar todavia.
    bool emit_route(const RouteDecl& route, Chunk& out);

    // Cuerpo de una funcion de usuario.
    bool emit_function(const FnDecl& fn, Chunk& out);

    // Metodo y constructor: se compilan como funciones con `this` de primer
    // parametro, asi que reusan la pila de marcos del VM sin nada especial.
    bool emit_method(const std::string& cls, const FnDecl& m, Chunk& out);
    bool emit_ctor(const std::string& cls, const std::vector<std::string>& fields,
                   const CtorDecl& ct, Chunk& out);

    // Compila una expresion suelta con `names` ya declarados como locales, en
    // ese orden.  Lo usan las reglas de `validate` —cada una se convierte en un
    // chunk diminuto que recibe los campos y devuelve un booleano— y las
    // expresiones de dentro de un {{ }} en una plantilla.
    //
    // Los tipos viajan con los nombres para que `nombre.mayusculas()` se pueda
    // rechazar aqui: sin ellos el emisor no sabe que `nombre` es un string y
    // la errata se descubre en produccion.
    bool emit_condition(const Expr& e, const std::vector<NombreTipado>& names,
                        Chunk& out);

    // Cuerpo de un `on error`: sin parametros, con el objeto `error` disponible.
    bool emit_error_handler(const ErrorDecl& decl, Chunk& out);

    // ── Fase 1 (COMPILACION-NATIVA.md): checker en paralelo ─────────────────
    //
    // Reproduce TODAS las comprobaciones que hace emit_expr/emit_call (mismo
    // texto de error, mismo orden), pero sin tocar chunk_ ni locals_: no emite
    // bytecode y no declara ninguna ranura (las unicas declare_local() de hoy
    // son temporales de codegen -- PreStep/PostStep sobre un campo o un
    // indice -- que un paso de solo comprobacion no necesita).  Por eso puede
    // correr sobre el `this` REAL de una compilacion en curso, en cualquier
    // orden respecto a emit_expr/emit_call, sin corromper la numeracion de
    // ranuras: es un lector de locals_, nunca un escritor.
    //
    // Los errores van a `shadow`, NUNCA a diags_: todavia no es la fuente de
    // diagnosticos (eso llega cuando el corte real conecte esto a Emitter),
    // asi que un error de aqui no debe duplicar el que ya produce
    // emit_expr/emit_call por su cuenta.
    //
    // Ademas de comprobar, CONSTRUYE y devuelve el IrExpr correspondiente
    // (nulo si algo no compilo -- ya se llamo a shadow.error en el sitio
    // exacto). El tipo de cada nodo es tipo_de(e), sin excepcion: nunca un
    // tipo mas preciso inventado aqui, porque eso seria funcionalidad nueva
    // y no una reproduccion de lo que ya hace el compilador. Publico porque
    // la verificacion (comparar shadow contra diags_ real, y el shape del
    // IrExpr devuelto, sobre el corpus de tests/casos) vive en un binario de
    // pruebas aparte; nada en el compilador real llama a esto todavia.
    IrExprPtr check_expr(const Expr& e, DiagnosticBag& shadow) const;
    IrExprPtr check_call(const Expr& e, bool awaited, DiagnosticBag& shadow) const;

    // Contrapartida de check_expr para emit_condition: mismo reinicio de
    // locals_/route_method_/scope_depth_, mismas declaraciones de `names`,
    // pero llamando a check_expr en vez de a emit_expr. Es la manera mas
    // directa de comparar check_expr contra la compilacion real sobre una
    // expresion suelta: emit_condition y check_condition se pueden llamar en
    // secuencia sobre el mismo Emitter porque las dos reinician su estado por
    // completo al entrar, igual que ya hace emit_condition hoy si se llama
    // mas de una vez.
    IrExprPtr check_condition(const Expr& e, const std::vector<NombreTipado>& names,
                              DiagnosticBag& shadow);

    // check_stmt/check_block: la misma idea que check_expr, pero para
    // sentencias -- reproducen emit_stmt/emit_block rama a rama y construyen
    // el IrStmt/IrBlock correspondiente (nulo/vacio en caso de error, mismo
    // criterio que check_expr). A diferencia de check_expr, SI declaran
    // ranuras (VarDecl, el `for` desazucarado, el nombre de un `catch`):
    // esas SI son contabilidad de nombres real, no un temporal de codegen, y
    // hace falta que el checker la lleve para que el Ident de una sentencia
    // posterior resuelva bien. Por construccion no puede correr sobre el
    // `this` de una emision real en curso (pisaria sus ranuras) -- de ahi
    // check_route/check_function/etc. como puntos de entrada propios, cada
    // uno reiniciando el estado exactamente como su contrapartida emit_*,
    // para poder llamarse en secuencia sobre el mismo Emitter (primero la
    // via real, luego la sombra) sin interferir.
    IrStmtPtr check_stmt(const Stmt& s, DiagnosticBag& shadow);
    // Vacio si algun sentencia del bloque fallo (ya se reporto en su sitio);
    // igual que hoy con `failed_`, un bloque a medio construir no se usa.
    IrBlock   check_block(const Block& body, DiagnosticBag& shadow);

    // `out_body`, si no es nulo, recibe el IrBlock construido (el mismo que
    // ya se descarta hoy en project.cpp: check_route/etc. lo usan solo para
    // saber si `shadow` crecio). Parametro opcional para no tocar ninguno de
    // los 9 sitios de project.cpp que ya llaman a esto -- lo usan las
    // pruebas que quieren inspeccionar la forma del IR construido.
    bool check_route(const RouteDecl& route, DiagnosticBag& shadow, IrBlock* out_body = nullptr);
    bool check_function(const FnDecl& fn, DiagnosticBag& shadow, IrBlock* out_body = nullptr);
    bool check_method(const std::string& cls, const FnDecl& m, DiagnosticBag& shadow,
                      IrBlock* out_body = nullptr);
    bool check_ctor(const std::string& cls, const std::vector<std::string>& fields,
                    const CtorDecl& ct, DiagnosticBag& shadow, IrBlock* out_body = nullptr);
    bool check_error_handler(const ErrorDecl& decl, DiagnosticBag& shadow,
                             IrBlock* out_body = nullptr);

private:
    DiagnosticBag&                       diags_;
    const FunctionSigs*                  functions_ = nullptr;
    const ClassSigs*                     classes_   = nullptr;
    const std::set<std::string>*         imports_   = nullptr;
    PlantillaCtx*                        plantillas_ = nullptr;
    Chunk*         chunk_ = nullptr;
    bool           failed_ = false;

    // Metodo de la ruta que se esta compilando: permite rechazar `sse.*` fuera
    // de una ruta sse al compilar, en vez de dejarlo para runtime.
    std::string route_method_;

    // El tipo declarado se guarda para poder resolver `u.metodo()` en
    // compilacion: en runtime una instancia es un Dict y no se distinguiria.
    struct Local { std::string name; int depth; Type type; };
    std::vector<Local> locals_;
    int                scope_depth_ = 0;

    // Saltos pendientes del bucle en curso.  Los dos se parchean al cerrarlo:
    // en un `for` el destino de `continue` es el incremento, que todavia no se
    // ha emitido cuando aparece el `continue` dentro del cuerpo.
    struct LoopCtx {
        std::vector<size_t> breaks;
        std::vector<size_t> continues;
    };
    std::vector<LoopCtx> loops_;

    void error(SourceLoc loc, std::string msg);

    int  declare_local(const std::string& name, SourceLoc loc,
                       Type type = Type::unknown());
    const Type& local_type(const std::string& name) const;

    // Cierto solo si se PUEDE demostrar que la expresion es de tipo int.  Ante
    // la duda dice que no: especializar de menos solo deja codigo generico,
    // especializar de mas seria un error.
    bool es_int(const Expr& e) const;
    int  resolve_local(const std::string& name) const;
    void begin_scope();
    void end_scope();

    void emit_block(const Block& body);
    void emit_stmt(const Stmt& s);
    void emit_expr(const Expr& e);
    void emitir_render_compilado(const Expr& e);
    // Tipo estatico de una expresion, o Type::unknown() si no se puede saber.
    // Solo mira lo que es evidente sin inferencia: un literal, o una variable
    // declarada.
    Type tipo_de(const Expr& e) const;
    // Comprueba un metodo contra la lista cerrada del tipo del receptor.
    // Devuelve false —y ya ha dado el error— si ese metodo no existe.
    bool comprobar_metodo_builtin(const Expr& e);
    // Idem para un campo, al leerlo y al escribirlo.
    bool comprobar_campo(const Expr& objeto, const std::string& campo, SourceLoc loc);
    void emit_call(const Expr& e, bool awaited);
    // Metodo cuyo receptor no tiene tipo conocido al compilar: se apila y el
    // despacho por tipo lo hace el VM.
    void emit_method_call_dynamic(const Expr& e);

    // Shadow de comprobar_campo/comprobar_metodo_builtin: misma logica, error
    // a `shadow` en vez de a diags_. Usados por check_expr/check_call.
    bool check_campo(const Expr& objeto, const std::string& campo, SourceLoc loc,
                     DiagnosticBag& shadow) const;
    bool check_metodo_builtin(const Expr& e, DiagnosticBag& shadow) const;
};

} // namespace lumen_script
