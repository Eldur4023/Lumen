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
    // pero llamando a check_expr en vez de a emit_expr.
    //
    // Lleva su propio `Chunk& out` -- igual que emit_condition -- aunque no
    // emita ni un opcode: declare_local() escribe chunk_->num_locals segun
    // avanza (la misma contabilidad que necesita el VM para dimensionar la
    // pila, la real, no una copia), asi que necesita un chunk_ valido desde
    // el primer momento y no puede depender de que alguien haya llamado antes
    // a emit_condition sobre el mismo Emitter para dejarlo puesto. Pasar el
    // MISMO chunk que ya se le paso a emit_condition (que es lo que hace hoy
    // el canario de project.cpp) es valido: declare_local() vuelve a anotar
    // los mismos nombres, pero num_locals ya no puede subir mas de lo que ya
    // subio, asi que no cambia nada observable.
    IrExprPtr check_condition(const Expr& e, const std::vector<NombreTipado>& names,
                              Chunk& out, DiagnosticBag& shadow);

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

    // Emisor puro que consume el IrBlock que devuelve check_block/check_route/
    // etc.: no comprueba nada, confia en que el IR ya paso por el checker.
    // Publico (a diferencia de emit_stmt/emit_expr/emit_call sobre IrExpr/
    // IrStmt, que son privados e internos a este) porque tests/
    // emit_ir_shadow.cpp lo llama directamente para probar que el bytecode
    // que produce se COMPORTA igual que el del emisor viejo, ejecutado de
    // verdad en el VM -- es exactamente lo que hara emit_function una vez
    // conectado, asi que la prueba lo replica desde fuera.
    void emit_block(const IrBlock& body);

    // `out`: mismo motivo que en check_condition (declare_local necesita un
    // chunk_ valido). `out_body`, si no es nulo, recibe el IrBlock construido
    // (el mismo que ya se descarta hoy en project.cpp: check_route/etc. lo
    // usan solo para saber si `shadow` crecio).
    bool check_route(const RouteDecl& route, Chunk& out, DiagnosticBag& shadow,
                     IrBlock* out_body = nullptr);
    bool check_function(const FnDecl& fn, Chunk& out, DiagnosticBag& shadow,
                        IrBlock* out_body = nullptr);
    bool check_method(const std::string& cls, const FnDecl& m, Chunk& out,
                      DiagnosticBag& shadow, IrBlock* out_body = nullptr);
    bool check_ctor(const std::string& cls, const std::vector<std::string>& fields,
                    const CtorDecl& ct, Chunk& out, DiagnosticBag& shadow,
                    IrBlock* out_body = nullptr);
    bool check_error_handler(const ErrorDecl& decl, Chunk& out, DiagnosticBag& shadow,
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

    // ── Fase 1: emisor que consume el IR (todavia sin conectar) ─────────────
    //
    // Contrapartida de emit_block/emit_stmt/emit_expr/emit_call que en vez de
    // Expr/Stmt del AST recibe IrExpr/IrStmt ya resueltos por check_expr/
    // check_stmt. No comprueba nada -- ni una llamada a error(), ni una
    // busqueda por nombre (resolve_local/native_id/is_reserved_object...):
    // confia en que el IR que recibe ya paso por el checker. El nombre se
    // reusa (sobrecarga por tipo del parametro) porque son la MISMA operacion
    // -- "emitir esta expresion/sentencia" -- vista desde dos entradas
    // distintas mientras dura la migracion; cuando el corte final sustituya
    // las llamadas Expr/Stmt por estas, los nombres ya son los que hay que
    // dejar. emit_block es publico (ver el comentario junto a check_block)
    // porque la prueba de equivalencia de ejecucion (tests/emit_ir_shadow.cpp)
    // necesita invocarlo directamente, tal como hara emit_function una vez
    // conectado.
    void emit_stmt(const IrStmt& s);
    void emit_expr(const IrExpr& e);
    void emit_call(const IrExpr& e);
    // Cierto solo si se PUEDE demostrar que el IrExpr es de tipo int -- misma
    // regla que es_int(), pero leyendo IrExpr::type (ya resuelto por
    // check_expr) en vez de local_type(): ningun IrExpr necesita volver a
    // resolver un nombre.
    bool es_int_ir(const IrExpr& e) const;
    // Los builtins que llegan aqui con un native_id ya resuelto
    // (ReservedMemberCall y BuiltinGlobalCall salvo render(), que se
    // compila aparte): misma cola compartida que tiene hoy emit_call.
    void emit_native_call(const IrExpr& e);
    // Metodo cuyo receptor no tiene tipo conocido al compilar (BuiltinMethodCall).
    void emit_method_call_dynamic(const IrExpr& e);
    void emitir_render_compilado(const IrExpr& e);

    // Shadow de comprobar_campo/comprobar_metodo_builtin: misma logica, error
    // a `shadow` en vez de a diags_. Usados por check_expr/check_call.
    bool check_campo(const Expr& objeto, const std::string& campo, SourceLoc loc,
                     DiagnosticBag& shadow) const;
    bool check_metodo_builtin(const Expr& e, DiagnosticBag& shadow) const;
};

} // namespace lumen_script
