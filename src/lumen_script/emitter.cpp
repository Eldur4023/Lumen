#include <lumen_script/emitter.hpp>
#include <fstream>
#include <filesystem>
#include <lumen_script/plantilla.hpp>
#include <lumen_script/natives.hpp>

#include <algorithm>

namespace lumen_script {

void Emitter::error(SourceLoc loc, std::string msg) {
    diags_.error(loc, std::move(msg));
    failed_ = true;
}

int Emitter::declare_local(const std::string& name, SourceLoc loc, Type type) {
    for (auto it = locals_.rbegin(); it != locals_.rend(); ++it) {
        if (it->depth < scope_depth_) break;
        if (it->name == name) {
            error(loc, "'" + name + "' ya esta declarada en este ambito");
            return static_cast<int>(std::distance(locals_.begin(), it.base()) - 1);
        }
    }
    locals_.push_back({name, scope_depth_, std::move(type)});
    int slot = static_cast<int>(locals_.size()) - 1;
    if (slot + 1 > chunk_->num_locals) chunk_->num_locals = slot + 1;
    chunk_->local_names.push_back(name);
    return slot;
}

const Type& Emitter::local_type(const std::string& name) const {
    static const Type kNone = Type::unknown();
    for (int i = static_cast<int>(locals_.size()) - 1; i >= 0; --i)
        if (locals_[static_cast<size_t>(i)].name == name)
            return locals_[static_cast<size_t>(i)].type;
    return kNone;
}

bool Emitter::es_int(const Expr& e) const {
    switch (e.kind) {
        case ExprKind::IntLit: return true;
        // OJO: comparacion por ortografia a proposito, no por Kind. Hoy
        // 'long x' NO activa el opcode especializado (solo 'int' literal lo
        // hace) -- es el comportamiento existente, que esta migracion no
        // cambia aunque la gramatica diga que int/long son el mismo tipo.
        case ExprKind::Ident:  return local_type(e.text).base_name() == "int";
        case ExprKind::Binary:
            if (e.text == "+" || e.text == "-" || e.text == "*")
                return e.lhs && e.rhs && es_int(*e.lhs) && es_int(*e.rhs);
            return false;
        default: return false;
    }
}

int Emitter::resolve_local(const std::string& name) const {
    for (int i = static_cast<int>(locals_.size()) - 1; i >= 0; --i)
        if (locals_[static_cast<size_t>(i)].name == name) return i;
    return -1;
}

void Emitter::begin_scope() { ++scope_depth_; }

void Emitter::end_scope() {
    --scope_depth_;
    // Las ranuras no se reciclan: el coste es una entrada mas en el vector de
    // locales y a cambio los indices son estables, lo que simplifica el VM.
    while (!locals_.empty() && locals_.back().depth > scope_depth_)
        locals_.pop_back();
}

// Los 6 puntos de entrada reales (emit_route/emit_function/emit_method/
// emit_ctor/emit_condition/emit_error_handler) delegan en su check_*
// correspondiente para construir el IR -- con diags_ real, no un shadow
// aparte, asi que check_* pasa a ser la UNICA fuente de diagnosticos -- y
// solo si eso tuvo exito llaman a emit_block/emit_expr (el emisor que
// consume el IR) para producir el bytecode. emit_expr/emit_stmt/emit_call/
// emit_block sobre Expr/Stmt y comprobar_campo/comprobar_metodo_builtin ya
// no los llama nadie desde aqui: siguen en el arbol porque tipo_de() (que SI
// se sigue usando, desde check_expr/check_call/check_campo) esta definida en
// terminos de Expr, no de IrExpr, y no vale la pena duplicarla -- pero un
// commit aparte los retira una vez confirmado que de verdad ya no hace falta
// ninguno.
// El `return !failed_` final (no `return true`) importa: emitir_render_
// compilado (el unico sitio que sigue pudiendo fallar durante la emision,
// no solo durante el chequeo -- ver su comentario) pone failed_ a true si
// la plantilla en si no compila, y eso solo se sabe DESPUES de emit_block.
bool Emitter::emit_route(const RouteDecl& route, Chunk& out) {
    IrBlock body;
    if (!check_route(route, out, diags_, &body)) { failed_ = true; return false; }
    failed_ = false;

    emit_block(body);

    // Un handler que se cae por el final no devuelve nada: el motor respondera
    // con lo que haya escrito un builtin, o 204 si no escribio nada.
    out.emit(Op::ReturnNull, route.loc);
    return !failed_;
}

bool Emitter::emit_function(const FnDecl& fn, Chunk& out) {
    IrBlock body;
    if (!check_function(fn, out, diags_, &body)) { failed_ = true; return false; }
    failed_ = false;

    emit_block(body);
    out.emit(Op::ReturnNull, fn.loc);
    return !failed_;
}

bool Emitter::emit_method(const std::string& cls, const FnDecl& m, Chunk& out) {
    IrBlock body;
    if (!check_method(cls, m, out, diags_, &body)) { failed_ = true; return false; }
    failed_ = false;

    emit_block(body);
    out.emit(Op::ReturnNull, m.loc);
    return !failed_;
}

bool Emitter::emit_ctor(const std::string& cls, const std::vector<std::string>& fields,
                        const CtorDecl& ct, Chunk& out) {
    IrBlock body;
    if (!check_ctor(cls, fields, ct, out, diags_, &body)) { failed_ = true; return false; }
    failed_ = false;

    // check_ctor ya declaro los parametros y `this` (en ese orden, antes de
    // cualquier begin_scope/end_scope que los pudiera hacer desaparecer de
    // locals_), asi que siguen ahi para resolverlos por nombre.
    int self = resolve_local("this");

    // La instancia arranca con todos los campos declarados a null, para que
    // acceder a uno que el constructor no toque de null y no falle.
    for (const auto& f : fields) {
        chunk_->emit(Op::Const, ct.loc, chunk_->add_constant(Value::str(f)));
        chunk_->emit(Op::Const, ct.loc, chunk_->add_constant(Value::null()));
    }
    chunk_->emit(Op::MakeDict, ct.loc, static_cast<uint32_t>(fields.size()));
    chunk_->emit(Op::StoreLocal, ct.loc, static_cast<uint32_t>(self));

    if (ct.has_body) {
        emit_block(body);
    } else {
        // Sin cuerpo: cada parametro va al campo de su mismo nombre.
        // check_ctor ya rechazo cualquier parametro que no sea un campo, asi
        // que si se llega aqui, todos lo son.
        for (const auto& p : ct.params) {
            chunk_->emit(Op::LoadLocal, ct.loc, static_cast<uint32_t>(self));
            int slot = resolve_local(p.name);
            chunk_->emit(Op::LoadLocal, ct.loc, static_cast<uint32_t>(slot));
            chunk_->emit(Op::SetMember, ct.loc, chunk_->add_constant(Value::str(p.name)));
            chunk_->emit(Op::Pop, ct.loc);
        }
    }

    chunk_->emit(Op::LoadLocal, ct.loc, static_cast<uint32_t>(self));
    chunk_->emit(Op::Return, ct.loc);
    return !failed_;
}

bool Emitter::emit_condition(const Expr& e, const std::vector<NombreTipado>& names,
                             Chunk& out) {
    IrExprPtr ir = check_condition(e, names, out, diags_);
    if (!ir) { failed_ = true; return false; }
    failed_ = false;

    emit_expr(*ir);
    out.emit(Op::Return, e.loc);
    return !failed_;
}

IrExprPtr Emitter::check_condition(const Expr& e, const std::vector<NombreTipado>& names,
                                   Chunk& out, DiagnosticBag& shadow) {
    chunk_        = &out;
    route_method_ = {};
    locals_.clear();
    loops_.clear();
    scope_depth_ = 0;

    for (const auto& n : names) declare_local(n.nombre, e.loc, Type::from_legacy_name(n.tipo));

    return check_expr(e, shadow);
}

// check_route/check_function/check_method/check_ctor/check_error_handler:
// contrapartida de emit_route/emit_function/emit_method/emit_ctor/
// emit_error_handler para check_stmt, con el mismo reinicio de estado y las
// mismas declaraciones de parametros/`this`. Devuelven si esta llamada en
// concreto no anadio ningun error a `shadow` (no si `shadow` esta vacio del
// todo: quien llama puede reusar el mismo DiagnosticBag para varios casos).
bool Emitter::check_route(const RouteDecl& route, Chunk& out, DiagnosticBag& shadow,
                          IrBlock* out_body) {
    size_t antes    = shadow.size();
    chunk_          = &out;
    route_method_   = route.method;
    locals_.clear();
    loops_.clear();
    scope_depth_ = 0;

    for (const auto& p : route.params) declare_local(p.name, p.loc, Type::from_declared(p.type));

    // Las guardas son "si no X, devuelve Y" -- el mismo IrStmtKind::Require
    // que ya usa `require`, y se ejecutan antes que el cuerpo (de fuera hacia
    // dentro), asi que van primero en el IrBlock que se devuelve.
    IrBlock guardas;
    for (const auto& g : route.guards) {
        if (!g.condition || !g.otherwise) continue;
        if (IrStmtPtr r = check_require_like(g.loc, *g.condition, *g.otherwise, shadow))
            guardas.push_back(std::move(r));
    }

    IrBlock body = check_block(route.body, shadow);
    if (out_body) {
        out_body->clear();
        out_body->reserve(guardas.size() + body.size());
        for (auto& g : guardas) out_body->push_back(std::move(g));
        for (auto& s : body)    out_body->push_back(std::move(s));
    }
    return shadow.size() == antes;
}

bool Emitter::check_function(const FnDecl& fn, Chunk& out, DiagnosticBag& shadow,
                             IrBlock* out_body) {
    size_t antes  = shadow.size();
    chunk_        = &out;
    route_method_ = "FN";
    locals_.clear();
    loops_.clear();
    scope_depth_ = 0;

    for (const auto& p : fn.params) declare_local(p.name, p.loc, Type::from_declared(p.type));
    IrBlock body = check_block(fn.body, shadow);
    if (out_body) *out_body = std::move(body);
    return shadow.size() == antes;
}

bool Emitter::check_method(const std::string& cls, const FnDecl& m, Chunk& out,
                           DiagnosticBag& shadow, IrBlock* out_body) {
    size_t antes  = shadow.size();
    chunk_        = &out;
    route_method_ = "FN";
    locals_.clear();
    loops_.clear();
    scope_depth_ = 0;

    declare_local("this", m.loc, Type::class_ref(cls));
    for (const auto& p : m.params) declare_local(p.name, p.loc, Type::from_declared(p.type));
    IrBlock body = check_block(m.body, shadow);
    if (out_body) *out_body = std::move(body);
    return shadow.size() == antes;
}

bool Emitter::check_ctor(const std::string& cls, const std::vector<std::string>& fields,
                         const CtorDecl& ct, Chunk& out, DiagnosticBag& shadow,
                         IrBlock* out_body) {
    size_t antes  = shadow.size();
    chunk_        = &out;
    route_method_ = "FN";
    locals_.clear();
    loops_.clear();
    scope_depth_ = 0;

    for (const auto& p : ct.params) declare_local(p.name, p.loc, Type::from_declared(p.type));
    declare_local("this", ct.loc, Type::class_ref(cls));

    if (ct.has_body) {
        IrBlock body = check_block(ct.body, shadow);
        if (out_body) *out_body = std::move(body);
    } else {
        for (const auto& p : ct.params) {
            if (std::find(fields.begin(), fields.end(), p.name) == fields.end())
                shadow.error(p.loc, "'" + p.name + "' no es un campo de '" + cls + "'");
        }
    }
    return shadow.size() == antes;
}

bool Emitter::check_error_handler(const ErrorDecl& decl, Chunk& out, DiagnosticBag& shadow,
                                  IrBlock* out_body) {
    size_t antes  = shadow.size();
    chunk_        = &out;
    route_method_ = "ERROR";
    locals_.clear();
    loops_.clear();
    scope_depth_ = 0;

    IrBlock body = check_block(decl.body, shadow);
    if (out_body) *out_body = std::move(body);
    return shadow.size() == antes;
}

bool Emitter::emit_error_handler(const ErrorDecl& decl, Chunk& out) {
    IrBlock body;
    if (!check_error_handler(decl, out, diags_, &body)) { failed_ = true; return false; }
    failed_ = false;

    emit_block(body);
    out.emit(Op::ReturnNull, decl.loc);
    return !failed_;
}

void Emitter::emit_block(const Block& body) {
    begin_scope();
    for (const auto& s : body) emit_stmt(*s);
    end_scope();
}

void Emitter::emit_stmt(const Stmt& s) {
    switch (s.kind) {
        case StmtKind::Return:
            if (s.value) { emit_expr(*s.value); chunk_->emit(Op::Return, s.loc); }
            else         { chunk_->emit(Op::ReturnNull, s.loc); }
            break;

        case StmtKind::ExprStmt:
            emit_expr(*s.value);
            chunk_->emit(Op::Pop, s.loc);
            break;

        case StmtKind::VarDecl: {
            if (s.value) emit_expr(*s.value);
            else         chunk_->emit(Op::Const, s.loc, chunk_->add_constant(Value::null()));
            int slot = declare_local(s.name, s.loc, Type::from_declared(s.type));
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(slot));
            break;
        }

        case StmtKind::Assign: {
            // `session.x = v` → __session_set("x", v).  Es la unica asignacion
            // a un miembro que existe; el resto de objetos reservados son de
            // solo lectura.
            if (s.target->kind == ExprKind::Member &&
                s.target->object->kind == ExprKind::Ident &&
                s.target->object->text == "session" &&
                resolve_local("session") < 0) {

                chunk_->emit(Op::Const, s.loc,
                             chunk_->add_constant(Value::str(s.target->text)));
                emit_expr(*s.value);
                chunk_->emit(Op::CallNative, s.loc,
                             (static_cast<uint32_t>(native_id("__session_set")) << 8) | 2u);
                chunk_->emit(Op::Pop, s.loc);
                return;
            }

            // `xs[0] = v` y `d["k"] = v`.
            if (s.target->kind == ExprKind::Index) {
                emit_expr(*s.target->object);
                emit_expr(*s.target->lhs);
                emit_expr(*s.value);
                chunk_->emit(Op::SetIndex, s.loc);
                chunk_->emit(Op::Pop, s.loc);
                return;
            }

            // `this.campo = v` y `objeto.campo = v`.
            if (s.target->kind == ExprKind::Member) {
                if (!comprobar_campo(*s.target->object, s.target->text, s.loc)) return;
                emit_expr(*s.target->object);
                emit_expr(*s.value);
                chunk_->emit(Op::SetMember, s.loc,
                             chunk_->add_constant(Value::str(s.target->text)));
                chunk_->emit(Op::Pop, s.loc);
                return;
            }

            if (s.target->kind != ExprKind::Ident) {
                error(s.loc, "solo se puede asignar a una variable o a un campo");
                return;
            }
            int slot = resolve_local(s.target->text);
            if (slot < 0) {
                error(s.target->loc, "'" + s.target->text + "' no esta declarada");
                return;
            }
            emit_expr(*s.value);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(slot));
            break;
        }

        case StmtKind::If: {
            emit_expr(*s.value);
            size_t to_else = chunk_->emit(Op::JumpIfFalse, s.loc);
            emit_block(s.body);

            if (!s.orelse.empty()) {
                size_t to_end = chunk_->emit(Op::Jump, s.loc);
                chunk_->patch(to_else, chunk_->here());
                emit_block(s.orelse);
                chunk_->patch(to_end, chunk_->here());
            } else {
                chunk_->patch(to_else, chunk_->here());
            }
            break;
        }

        case StmtKind::While: {
            size_t start = chunk_->here();
            emit_expr(*s.value);
            size_t to_end = chunk_->emit(Op::JumpIfFalse, s.loc);

            loops_.push_back({});
            emit_block(s.body);
            // En un while, `continue` vuelve a evaluar la condicion.
            for (size_t j : loops_.back().continues) chunk_->patch(j, start);
            chunk_->emit(Op::Jump, s.loc, static_cast<uint32_t>(start));
            chunk_->patch(to_end, chunk_->here());

            for (size_t j : loops_.back().breaks) chunk_->patch(j, chunk_->here());
            loops_.pop_back();
            break;
        }

        // `require X else Y` es azucar de `if not X: return Y`.  Se emite tal
        // cual: no hay opcode propio ni concepto de middleware en el VM.
        case StmtKind::Require: {
            emit_expr(*s.value);
            size_t to_ok = chunk_->emit(Op::JumpIfFalse, s.loc);
            size_t skip  = chunk_->emit(Op::Jump, s.loc);
            chunk_->patch(to_ok, chunk_->here());
            emit_expr(*s.target);
            chunk_->emit(Op::Return, s.loc);
            chunk_->patch(skip, chunk_->here());
            break;
        }

        case StmtKind::Break:
            if (loops_.empty()) { error(s.loc, "'break' fuera de un bucle"); return; }
            loops_.back().breaks.push_back(chunk_->emit(Op::Jump, s.loc));
            break;

        case StmtKind::Continue:
            if (loops_.empty()) { error(s.loc, "'continue' fuera de un bucle"); return; }
            loops_.back().continues.push_back(chunk_->emit(Op::Jump, s.loc));
            break;

        // `for T x in xs:` se desazucara a un indice sobre la lista.  Las
        // ranuras auxiliares llevan un espacio en el nombre, que ningun
        // identificador de Lumen Script puede contener: asi nunca chocan con las del
        // usuario ni entre dos bucles anidados.
        case StmtKind::For: {
            begin_scope();

            emit_expr(*s.target);
            chunk_->emit(Op::IterList, s.loc);
            int items = declare_local(" items", s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(items));

            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(items));
            chunk_->emit(Op::CallNative, s.loc,
                         (static_cast<uint32_t>(native_id("len")) << 8) | 1u);
            int count = declare_local(" count", s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(count));

            chunk_->emit(Op::Const, s.loc, chunk_->add_constant(Value::integer(0)));
            int index = declare_local(" index", s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(index));

            int var = declare_local(s.name, s.loc, Type::from_declared(s.type));

            size_t start = chunk_->here();
            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(index));
            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(count));
            chunk_->emit(Op::Lt, s.loc);
            size_t to_end = chunk_->emit(Op::JumpIfFalse, s.loc);

            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(items));
            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(index));
            chunk_->emit(Op::GetIndex, s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(var));

            loops_.push_back({});
            emit_block(s.body);

            // `continue` salta al incremento, no al principio: si no, el bucle
            // no avanzaria nunca.
            size_t step = chunk_->here();
            for (size_t j : loops_.back().continues) chunk_->patch(j, step);
            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(index));
            chunk_->emit(Op::Const, s.loc, chunk_->add_constant(Value::integer(1)));
            chunk_->emit(Op::Add, s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(index));
            chunk_->emit(Op::Jump, s.loc, static_cast<uint32_t>(start));

            chunk_->patch(to_end, chunk_->here());
            for (size_t j : loops_.back().breaks) chunk_->patch(j, chunk_->here());
            loops_.pop_back();

            end_scope();
            break;
        }

        case StmtKind::Try: {
            TryRange range;
            range.begin = chunk_->here();
            emit_block(s.body);
            range.end = chunk_->here();

            size_t to_end = chunk_->emit(Op::Jump, s.loc);
            range.catch_pc = chunk_->here();
            chunk_->try_ranges.push_back(range);

            // El error llega en la cima como un Dict con `message`.
            begin_scope();
            if (!s.name.empty()) {
                int slot = declare_local(s.name, s.loc);
                chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(slot));
            } else {
                chunk_->emit(Op::Pop, s.loc);
            }
            for (const auto& st : s.orelse) emit_stmt(*st);
            end_scope();

            chunk_->patch(to_end, chunk_->here());
            break;
        }
    }
}

// Traduce un StmtKind al IrStmtKind correspondiente -- ver el comentario de
// ir_kind_de: un switch explicito, no un static_cast, para que un StmtKind
// nuevo sin actualizar esto avise en compilacion.
static IrStmtKind ir_stmt_kind_de(StmtKind k) {
    switch (k) {
        case StmtKind::Return:   return IrStmtKind::Return;
        case StmtKind::ExprStmt: return IrStmtKind::ExprStmt;
        case StmtKind::VarDecl:  return IrStmtKind::VarDecl;
        case StmtKind::Assign:   return IrStmtKind::Assign;
        case StmtKind::If:       return IrStmtKind::If;
        case StmtKind::While:    return IrStmtKind::While;
        case StmtKind::For:      return IrStmtKind::For;
        case StmtKind::Require:  return IrStmtKind::Require;
        case StmtKind::Try:      return IrStmtKind::Try;
        case StmtKind::Break:    return IrStmtKind::Break;
        case StmtKind::Continue: return IrStmtKind::Continue;
    }
    return IrStmtKind::Return; // inalcanzable si el switch de arriba es exhaustivo
}

// Compartido por check_stmt (Require) y check_route (guardas de grupo): las
// dos son "si no `cond`, devuelve `otherwise`", el mismo IrStmtKind::Require.
IrStmtPtr Emitter::check_require_like(SourceLoc loc, const Expr& cond, const Expr& otherwise,
                                      DiagnosticBag& shadow) const {
    IrExprPtr c = check_expr(cond, shadow);
    IrExprPtr e = check_expr(otherwise, shadow);
    if (!c || !e) return nullptr;
    auto r = std::make_unique<IrStmt>();
    r->kind   = IrStmtKind::Require;
    r->loc    = loc;
    r->value  = std::move(c);
    r->target = std::move(e);
    return r;
}

// Shadow de emit_block/emit_stmt (fase 1, checker en paralelo -- ver
// emitter.hpp). Mismo orden de comprobaciones, mismo texto, sin tocar chunk_.
// A diferencia de check_expr, SI llama a declare_local/begin_scope/end_scope:
// VarDecl, el `for` desazucarado y el nombre de un `catch` son contabilidad
// de nombres real (no un temporal de codegen), y hace falta reproducirla para
// que el Ident de una sentencia posterior resuelva a la ranura correcta.
//
// check_block SIEMPRE visita las sentencias que tiene, aunque alguna falle:
// eso es lo que permite reportar todos los errores de un bloque en una sola
// pasada, igual que hace hoy emit_stmt. Una sentencia que fallo (nullptr) se
// omite del IrBlock resultante -- silenciosamente, no es un error nuevo, ya
// se reporto en su sitio -- asi que un IrBlock nunca lleva un hueco nulo. Un
// If/While/For cuya condicion o iterable fallo tampoco se propaga (devuelve
// nullptr el mismo), aunque su cuerpo se haya revisado entero para no perder
// los errores que haya dentro; un Try nunca falla por cuenta propia, porque
// no tiene una expresion propia que comprobar, solo delega en sus bloques.
IrBlock Emitter::check_block(const Block& body, DiagnosticBag& shadow) {
    begin_scope();
    IrBlock out;
    for (const auto& s : body) {
        IrStmtPtr st = check_stmt(*s, shadow);
        if (st) out.push_back(std::move(st));
    }
    end_scope();
    return out;
}

IrStmtPtr Emitter::check_stmt(const Stmt& s, DiagnosticBag& shadow) {
    auto nodo = [&]() {
        auto r = std::make_unique<IrStmt>();
        r->kind = ir_stmt_kind_de(s.kind);
        r->loc  = s.loc;
        return r;
    };

    switch (s.kind) {
        case StmtKind::Return: {
            auto r = nodo();
            if (s.value) {
                IrExprPtr v = check_expr(*s.value, shadow);
                if (!v) return nullptr;
                r->value = std::move(v);
            }
            return r;
        }

        case StmtKind::ExprStmt: {
            IrExprPtr v = check_expr(*s.value, shadow);
            if (!v) return nullptr;
            auto r = nodo();
            r->value = std::move(v);
            return r;
        }

        case StmtKind::VarDecl: {
            IrExprPtr v;
            if (s.value) {
                v = check_expr(*s.value, shadow);
                if (!v) return nullptr;
            }
            int slot = declare_local(s.name, s.loc, Type::from_declared(s.type));
            auto r = nodo();
            r->value     = std::move(v);
            r->name      = s.name;
            r->decl_type = Type::from_declared(s.type);
            r->slot      = slot;
            return r;
        }

        case StmtKind::Assign: {
            // `session.x = v` → __session_set("x", v).
            if (s.target->kind == ExprKind::Member &&
                s.target->object->kind == ExprKind::Ident &&
                s.target->object->text == "session" &&
                resolve_local("session") < 0) {
                IrExprPtr v = check_expr(*s.value, shadow);
                if (!v) return nullptr;
                auto r = nodo();
                r->assign_target = IrAssignTarget::Session;
                r->assign_field  = s.target->text;
                r->value = std::move(v);
                return r;
            }

            // `xs[0] = v` y `d["k"] = v`.
            if (s.target->kind == ExprKind::Index) {
                IrExprPtr obj = check_expr(*s.target->object, shadow);
                IrExprPtr idx = check_expr(*s.target->lhs, shadow);
                IrExprPtr val = check_expr(*s.value, shadow);
                if (!obj || !idx || !val) return nullptr;
                auto r = nodo();
                r->assign_target = IrAssignTarget::Index;
                r->assign_object = std::move(obj);
                r->assign_index  = std::move(idx);
                r->value = std::move(val);
                return r;
            }

            // `this.campo = v` y `objeto.campo = v`.
            if (s.target->kind == ExprKind::Member) {
                if (!check_campo(*s.target->object, s.target->text, s.loc, shadow)) return nullptr;
                IrExprPtr obj = check_expr(*s.target->object, shadow);
                IrExprPtr val = check_expr(*s.value, shadow);
                if (!obj || !val) return nullptr;
                auto r = nodo();
                r->assign_target = IrAssignTarget::Member;
                r->assign_object = std::move(obj);
                r->assign_field  = s.target->text;
                r->value = std::move(val);
                return r;
            }

            if (s.target->kind != ExprKind::Ident) {
                shadow.error(s.loc, "solo se puede asignar a una variable o a un campo");
                return nullptr;
            }
            int slot = resolve_local(s.target->text);
            if (slot < 0) {
                shadow.error(s.target->loc, "'" + s.target->text + "' no esta declarada");
                return nullptr;
            }
            IrExprPtr val = check_expr(*s.value, shadow);
            if (!val) return nullptr;
            auto r = nodo();
            r->assign_target = IrAssignTarget::Local;
            r->assign_slot   = slot;
            r->value = std::move(val);
            return r;
        }

        case StmtKind::If: {
            IrExprPtr cond = check_expr(*s.value, shadow);
            IrBlock   then = check_block(s.body, shadow);
            IrBlock   orelse;
            if (!s.orelse.empty()) orelse = check_block(s.orelse, shadow);
            if (!cond) return nullptr;
            auto r = nodo();
            r->value  = std::move(cond);
            r->body   = std::move(then);
            r->orelse = std::move(orelse);
            return r;
        }

        case StmtKind::While: {
            IrExprPtr cond = check_expr(*s.value, shadow);
            loops_.push_back({});
            IrBlock body = check_block(s.body, shadow);
            loops_.pop_back();
            if (!cond) return nullptr;
            auto r = nodo();
            r->value = std::move(cond);
            r->body  = std::move(body);
            return r;
        }

        case StmtKind::Require:
            return check_require_like(s.loc, *s.value, *s.target, shadow);

        case StmtKind::Break:
            if (loops_.empty()) { shadow.error(s.loc, "'break' fuera de un bucle"); return nullptr; }
            return nodo();

        case StmtKind::Continue:
            if (loops_.empty()) { shadow.error(s.loc, "'continue' fuera de un bucle"); return nullptr; }
            return nodo();

        case StmtKind::For: {
            begin_scope();
            IrExprPtr iterable = check_expr(*s.target, shadow);
            declare_local(" items", s.loc);
            declare_local(" count", s.loc);
            declare_local(" index", s.loc);
            int var = declare_local(s.name, s.loc, Type::from_declared(s.type));

            loops_.push_back({});
            IrBlock body = check_block(s.body, shadow);
            loops_.pop_back();
            end_scope();

            if (!iterable) return nullptr;
            auto r = nodo();
            r->target    = std::move(iterable);
            r->name      = s.name;
            r->decl_type = Type::from_declared(s.type);
            r->slot      = var;
            r->body      = std::move(body);
            return r;
        }

        case StmtKind::Try: {
            IrBlock body = check_block(s.body, shadow);
            begin_scope();
            int slot = -1;
            if (!s.name.empty()) slot = declare_local(s.name, s.loc);
            IrBlock orelse;
            for (const auto& st : s.orelse) {
                IrStmtPtr ir = check_stmt(*st, shadow);
                if (ir) orelse.push_back(std::move(ir));
            }
            end_scope();

            auto r = nodo();
            r->body   = std::move(body);
            r->name   = s.name;
            r->slot   = slot;
            r->orelse = std::move(orelse);
            return r;
        }
    }
    return nullptr; // inalcanzable si el switch de arriba es exhaustivo
}

void Emitter::emit_expr(const Expr& e) {
    switch (e.kind) {
        case ExprKind::StringLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::str(e.text)));
            break;
        case ExprKind::IntLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::integer(e.int_value)));
            break;
        case ExprKind::FloatLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::real(e.float_value)));
            break;
        case ExprKind::BoolLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::boolean(e.bool_value)));
            break;
        case ExprKind::NullLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::null()));
            break;

        case ExprKind::Ident: {
            int slot = resolve_local(e.text);
            if (slot < 0) {
                if (native_id(e.text) >= 0)
                    error(e.loc, "'" + e.text + "' es un builtin: hay que llamarlo, "
                                 "no usarlo como valor");
                else
                    error(e.loc, "'" + e.text + "' no esta declarada");
                return;
            }
            chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(slot));
            break;
        }

        case ExprKind::Unary:
            emit_expr(*e.lhs);
            chunk_->emit(e.text == "not" ? Op::Not : Op::Neg, e.loc);
            break;

        // and/or cortocircuitan: se mira la cima sin consumirla y se salta con
        // ella puesta, que es justo el valor del resultado.
        case ExprKind::Binary: {
            if (e.text == "and" || e.text == "or") {
                emit_expr(*e.lhs);
                size_t j = chunk_->emit(e.text == "and" ? Op::JumpIfFalsePeek
                                                        : Op::JumpIfTruePeek, e.loc);
                emit_expr(*e.rhs);
                chunk_->patch(j, chunk_->here());
                break;
            }

            emit_expr(*e.lhs);
            emit_expr(*e.rhs);
            Op op = Op::Add;
            if      (e.text == "+")  op = Op::Add;
            else if (e.text == "-")  op = Op::Sub;
            else if (e.text == "*")  op = Op::Mul;
            else if (e.text == "/")  op = Op::Div;
            else if (e.text == "%")  op = Op::Mod;
            else if (e.text == "==") op = Op::Eq;
            else if (e.text == "!=") op = Op::Ne;
            else if (e.text == "<")  op = Op::Lt;
            else if (e.text == "<=") op = Op::Le;
            else if (e.text == ">")  op = Op::Gt;
            else if (e.text == ">=") op = Op::Ge;
            else { error(e.loc, "operador no soportado: " + e.text); return; }

            // Lumen Script declara los tipos, asi que lo que el VM generico averigua en
            // cada vuelta se sabe aqui una sola vez.
            if (es_int(*e.lhs) && es_int(*e.rhs)) {
                switch (op) {
                    case Op::Add: op = Op::AddInt; break;
                    case Op::Sub: op = Op::SubInt; break;
                    case Op::Mul: op = Op::MulInt; break;
                    case Op::Lt:  op = Op::LtInt;  break;
                    case Op::Le:  op = Op::LeInt;  break;
                    case Op::Gt:  op = Op::GtInt;  break;
                    case Op::Ge:  op = Op::GeInt;  break;
                    default: break;
                }
            }
            chunk_->emit(op, e.loc);
            break;
        }

        case ExprKind::Ternary: {
            emit_expr(*e.object);
            size_t to_else = chunk_->emit(Op::JumpIfFalse, e.loc);
            emit_expr(*e.lhs);
            size_t to_end = chunk_->emit(Op::Jump, e.loc);
            chunk_->patch(to_else, chunk_->here());
            emit_expr(*e.rhs);
            chunk_->patch(to_end, chunk_->here());
            break;
        }

        case ExprKind::ListLit:
            for (const auto& item : e.items) emit_expr(*item);
            chunk_->emit(Op::MakeList, e.loc, static_cast<uint32_t>(e.items.size()));
            break;

        case ExprKind::DictLit:
            for (const auto& entry : e.entries) {
                emit_expr(*entry.key);
                emit_expr(*entry.value);
            }
            chunk_->emit(Op::MakeDict, e.loc, static_cast<uint32_t>(e.entries.size()));
            break;

        case ExprKind::Index:
            emit_expr(*e.object);
            emit_expr(*e.lhs);
            chunk_->emit(Op::GetIndex, e.loc);
            break;

        // `sse.open` no es un campo de un diccionario: es un objeto reservado,
        // y se resuelve al builtin que lo implementa.
        case ExprKind::Member: {
            // `session.x` admite cualquier nombre: es un almacen, no un
            // objeto con miembros fijos.  Se traduce a __session_get("x").
            if (e.object->kind == ExprKind::Ident &&
                e.object->text == "session" &&
                resolve_local("session") < 0 &&
                member_native_id("session", e.text) < 0) {

                chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::str(e.text)));
                chunk_->emit(Op::CallNative, e.loc,
                             (static_cast<uint32_t>(native_id("__session_get")) << 8) | 1u);
                return;
            }

            if (e.object->kind == ExprKind::Ident &&
                resolve_local(e.object->text) < 0 &&
                is_reserved_object(e.object->text)) {

                int id = member_native_id(e.object->text, e.text);
                if (id < 0) {
                    error(e.loc, "'" + e.object->text + "' no tiene un miembro '" +
                                 e.text + "'");
                    return;
                }
                if (native_at(id).min_args > 0) {
                    error(e.loc, "'" + e.object->text + "." + e.text +
                                 "' es una operacion: hay que llamarla con ()");
                    return;
                }
                if ((e.object->text == "sse" && route_method_ != "SSE") ||
                    (e.object->text == "ws"  && route_method_ != "WS")) {
                    error(e.loc, "'" + e.object->text + "' solo existe dentro de "
                                 "una ruta " + e.object->text);
                    return;
                }
                if (e.object->text == "error" && route_method_ != "ERROR") {
                    error(e.loc, "'error' solo existe dentro de un 'on error'");
                    return;
                }
                chunk_->emit(Op::CallNative, e.loc,
                             static_cast<uint32_t>(id) << 8);
                return;
            }
            if (!comprobar_campo(*e.object, e.text, e.loc)) return;

            emit_expr(*e.object);
            chunk_->emit(Op::GetMember, e.loc,
                         chunk_->add_constant(Value::str(e.text)));
            break;
        }

        case ExprKind::Call:
            emit_call(e, /*awaited=*/false);
            break;

        // `await` solo tiene sentido sobre una llamada a un builtin que
        // suspende.  Cualquier otra cosa se rechaza aqui, no en runtime.
        // ++x / x++ sobre una variable: se lee dos veces, que es mas barato que
        // duplicar en la pila y no necesita opcode nuevo.
        //
        // Sobre un campo, el receptor se guarda en una ranura auxiliar: `o.f++`
        // no puede evaluar `o` dos veces, porque `o` puede tener efectos.
        case ExprKind::PreStep:
        case ExprKind::PostStep: {
            bool post = (e.kind == ExprKind::PostStep);
            Op   op   = (e.text == "+") ? Op::Add : Op::Sub;
            const Expr& tgt = *e.lhs;

            auto one = [&] {
                chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::integer(1)));
            };

            if (tgt.kind == ExprKind::Ident) {
                int slot = resolve_local(tgt.text);
                if (slot < 0) { error(tgt.loc, "'" + tgt.text + "' no esta declarada"); return; }
                uint32_t u = static_cast<uint32_t>(slot);

                if (post) chunk_->emit(Op::LoadLocal, e.loc, u);   // valor anterior
                chunk_->emit(Op::LoadLocal, e.loc, u);
                one();
                chunk_->emit(op, e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, u);
                if (!post) chunk_->emit(Op::LoadLocal, e.loc, u);  // valor nuevo
                break;
            }

            if (tgt.kind == ExprKind::Member) {
                begin_scope();
                uint32_t name_k = chunk_->add_constant(Value::str(tgt.text));

                emit_expr(*tgt.object);
                int obj = declare_local(" recep", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(obj));

                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(obj));
                chunk_->emit(Op::GetMember, e.loc, name_k);
                int prev = declare_local(" previo", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(prev));

                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(obj));
                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(prev));
                one();
                chunk_->emit(op, e.loc);
                chunk_->emit(Op::SetMember, e.loc, name_k);

                // SetMember deja el valor nuevo en la cima.
                if (post) {
                    chunk_->emit(Op::Pop, e.loc);
                    chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(prev));
                }
                end_scope();
                break;
            }

            if (tgt.kind == ExprKind::Index) {
                begin_scope();
                emit_expr(*tgt.object);
                int cont = declare_local(" contened", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(cont));

                emit_expr(*tgt.lhs);
                int idx = declare_local(" indice", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(idx));

                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(cont));
                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(idx));
                chunk_->emit(Op::GetIndex, e.loc);
                int prev = declare_local(" previo", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(prev));

                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(cont));
                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(idx));
                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(prev));
                one();
                chunk_->emit(op, e.loc);
                chunk_->emit(Op::SetIndex, e.loc);

                // SetIndex deja el valor nuevo en la cima.
                if (post) {
                    chunk_->emit(Op::Pop, e.loc);
                    chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(prev));
                }
                end_scope();
                break;
            }

            error(e.loc, "'++' y '--' solo se aplican a una variable, un campo "
                         "o un elemento indexado");
            break;
        }

        case ExprKind::Await: {
            if (!e.lhs || e.lhs->kind != ExprKind::Call) {
                error(e.loc, "'await' solo se aplica a una llamada asincrona "
                             "(de momento: sleep)");
                return;
            }
            emit_call(*e.lhs, /*awaited=*/true);
            break;
        }

        case ExprKind::This: {
            int slot = resolve_local("this");
            if (slot < 0) {
                error(e.loc, "'this' solo existe dentro de un metodo o un constructor");
                return;
            }
            chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(slot));
            break;
        }
    }
}

// Un campo sobre un receptor de tipo conocido.  Vale tanto para leerlo como
// para escribirlo: los campos de una clase son los declarados, y si no se
// comprobara la escritura se podria crear uno que luego no se deja leer.
//
// Sin la comprobacion, leer un campo que no existe devuelve null en silencio y
// el fallo aparece paginas mas adelante, lejos de la errata.
bool Emitter::comprobar_campo(const Expr& objeto, const std::string& campo,
                              SourceLoc loc) {
    const Type tr = tipo_de(objeto);
    if (tr.is_unknown()) return true;
    const std::string base = tr.base_name();

    if (classes_) {
        auto it = classes_->find(base);
        if (it != classes_->end()) {
            const auto& f = it->second.fields;
            if (std::find(f.begin(), f.end(), campo) != f.end()) return true;
            if (it->second.methods.count(campo)) {
                error(loc, "'" + base + "." + campo + "' es un metodo: "
                           "hay que llamarlo con ()");
            } else {
                std::string hay;
                for (const auto& n : f) hay += (hay.empty() ? "" : ", ") + n;
                error(loc, "'" + base + "' no tiene un campo '" + campo + "'" +
                           (hay.empty() ? "" : "; tiene " + hay));
            }
            return false;
        }
    }
    // Un escalar o una List no tienen campos, se llame como se llame lo que se
    // les pida.  Un Dict si: ahi cualquier clave es valida.
    if (metodos_de(base) && base != "Dict") {
        error(loc, "'" + campo + "' sobre " + base + ", que no tiene campos");
        return false;
    }
    return true;
}

// Solo lo evidente: un literal, o una variable con tipo declarado. No hay
// inferencia, asi que ante la duda devuelve Type::unknown() y no se
// comprueba nada.
Type Emitter::tipo_de(const Expr& e) const {
    switch (e.kind) {
        case ExprKind::StringLit: return Type::primitive(Type::Kind::String);
        case ExprKind::IntLit:    return Type::primitive(Type::Kind::Int);
        case ExprKind::FloatLit:  return Type::primitive(Type::Kind::Float);
        case ExprKind::BoolLit:   return Type::primitive(Type::Kind::Bool);
        case ExprKind::Ident:     return local_type(e.text);
        case ExprKind::This:      return local_type("this");
        // Metodo builtin sobre un receptor de tipo conocido: la cadena sigue.
        case ExprKind::Call: {
            if (!e.object || e.object->kind != ExprKind::Member) return Type::unknown();
            const Type recv = tipo_de(*e.object->object);
            const auto* lista = metodos_de(recv.base_name());
            if (!lista) return Type::unknown();
            for (const auto& m : *lista)
                if (e.object->text == m.nombre)
                    return m.devuelve ? Type::from_legacy_name(m.devuelve) : recv;
            return Type::unknown();
        }
        default:                  return Type::unknown();
    }
}

bool Emitter::comprobar_metodo_builtin(const Expr& e) {
    const Type recv = tipo_de(*e.object->object);
    const auto* lista = metodos_de(recv.base_name());
    if (!lista) return true;                  // tipo sin lista cerrada

    const std::string& metodo = e.object->text;
    const MetodoBuiltin* def = nullptr;
    for (const auto& m : *lista)
        if (metodo == m.nombre) { def = &m; break; }

    if (!def) {
        std::string hay;
        for (const auto& m : *lista) hay += (hay.empty() ? "" : ", ") + std::string(m.nombre);
        error(e.object->loc, "los valores de tipo " + recv.base_name() +
                             " no tienen el metodo '" + metodo + "'; tienen " + hay);
        return false;
    }

    size_t argc = 0, named = 0;
    for (const auto& a : e.args) (a.name.empty() ? argc : named)++;
    if (named > 0) ++argc;                    // van agrupados en un Dict

    if (static_cast<int>(argc) < def->min_args ||
        static_cast<int>(argc) > def->max_args) {
        std::string espera = std::to_string(def->min_args);
        if (def->max_args != def->min_args) espera += "-" + std::to_string(def->max_args);
        error(e.loc, "'" + metodo + "()' espera " + espera +
                     " argumento(s), pero recibe " + std::to_string(argc));
        return false;
    }
    return true;
}

// Shadow de comprobar_campo: misma logica letra por letra, error a `shadow`
// en vez de a diags_ (fase 1, checker en paralelo -- ver emitter.hpp).
bool Emitter::check_campo(const Expr& objeto, const std::string& campo, SourceLoc loc,
                          DiagnosticBag& shadow) const {
    const Type tr = tipo_de(objeto);
    if (tr.is_unknown()) return true;
    const std::string base = tr.base_name();

    if (classes_) {
        auto it = classes_->find(base);
        if (it != classes_->end()) {
            const auto& f = it->second.fields;
            if (std::find(f.begin(), f.end(), campo) != f.end()) return true;
            if (it->second.methods.count(campo)) {
                shadow.error(loc, "'" + base + "." + campo + "' es un metodo: "
                           "hay que llamarlo con ()");
            } else {
                std::string hay;
                for (const auto& n : f) hay += (hay.empty() ? "" : ", ") + n;
                shadow.error(loc, "'" + base + "' no tiene un campo '" + campo + "'" +
                           (hay.empty() ? "" : "; tiene " + hay));
            }
            return false;
        }
    }
    if (metodos_de(base) && base != "Dict") {
        shadow.error(loc, "'" + campo + "' sobre " + base + ", que no tiene campos");
        return false;
    }
    return true;
}

// Shadow de comprobar_metodo_builtin: idem.
bool Emitter::check_metodo_builtin(const Expr& e, DiagnosticBag& shadow) const {
    const Type recv = tipo_de(*e.object->object);
    const auto* lista = metodos_de(recv.base_name());
    if (!lista) return true;

    const std::string& metodo = e.object->text;
    const MetodoBuiltin* def = nullptr;
    for (const auto& m : *lista)
        if (metodo == m.nombre) { def = &m; break; }

    if (!def) {
        std::string hay;
        for (const auto& m : *lista) hay += (hay.empty() ? "" : ", ") + std::string(m.nombre);
        shadow.error(e.object->loc, "los valores de tipo " + recv.base_name() +
                             " no tienen el metodo '" + metodo + "'; tienen " + hay);
        return false;
    }

    size_t argc = 0, named = 0;
    for (const auto& a : e.args) (a.name.empty() ? argc : named)++;
    if (named > 0) ++argc;

    if (static_cast<int>(argc) < def->min_args ||
        static_cast<int>(argc) > def->max_args) {
        std::string espera = std::to_string(def->min_args);
        if (def->max_args != def->min_args) espera += "-" + std::to_string(def->max_args);
        shadow.error(e.loc, "'" + metodo + "()' espera " + espera +
                     " argumento(s), pero recibe " + std::to_string(argc));
        return false;
    }
    return true;
}

void Emitter::emit_method_call_dynamic(const Expr& e) {
    emit_expr(*e.object->object);
    size_t argc = 0, named = 0;
    for (const auto& a : e.args) {
        if (!a.name.empty()) { ++named; continue; }
        emit_expr(*a.value);
        ++argc;
    }
    // Los argumentos con nombre se agrupan en un Dict que ocupa el ultimo hueco
    // posicional, igual que en render().
    if (named > 0) {
        for (const auto& a : e.args) {
            if (a.name.empty()) continue;
            chunk_->emit(Op::Const, a.loc, chunk_->add_constant(Value::str(a.name)));
            emit_expr(*a.value);
        }
        chunk_->emit(Op::MakeDict, e.loc, static_cast<uint32_t>(named));
        ++argc;
    }
    if (argc > 255) { error(e.loc, "demasiados argumentos"); return; }
    chunk_->emit(Op::CallMethod, e.loc,
                 (chunk_->add_constant(Value::str(e.object->text)) << 8) |
                 static_cast<uint32_t>(argc));
}

void Emitter::emit_call(const Expr& e, bool awaited) {
    if (!e.object) { error(e.loc, "llamada sin destino"); return; }

    std::string name;
    int         id = -1;

    // sse.send(...) — miembro de un objeto reservado.
    if (e.object->kind == ExprKind::Member &&
        e.object->object->kind == ExprKind::Ident &&
        resolve_local(e.object->object->text) < 0 &&
        is_reserved_object(e.object->object->text)) {

        name = e.object->object->text + "." + e.object->text;
        id   = member_native_id(e.object->object->text, e.object->text);
        if (id < 0) {
            error(e.object->loc, "'" + e.object->object->text +
                                 "' no tiene un miembro '" + e.object->text + "'");
            return;
        }
        const std::string& obj = e.object->object->text;

        // Modulos: `sqlite.query(...)` exige `import sqlite`, y el nombre del
        // modulo viaja como primer argumento para que un solo builtin sirva
        // para los tres.
        bool is_module = is_db_module(obj);
        if (is_module && (!imports_ || !imports_->count(obj))) {
            error(e.object->loc, "falta 'import " + obj + "' para poder usar '" +
                                 obj + "." + e.object->text + "'");
            return;
        }
        if (is_module) {
            const NativeDef& mdef = native_at(id);
            if (!awaited) {
                error(e.loc, "'" + obj + "." + e.object->text + "()' es asincrono: "
                             "hay que escribir 'await " + obj + "." + e.object->text +
                             "(...)'");
                return;
            }
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::str(obj)));
            size_t argc = 1;
            for (const auto& a : e.args) {
                if (!a.name.empty()) {
                    error(a.loc, "las consultas no admiten argumentos con nombre");
                    return;
                }
                emit_expr(*a.value);
                ++argc;
            }
            if (argc < static_cast<size_t>(mdef.min_args)) {
                error(e.loc, "'" + obj + "." + e.object->text +
                             "()' espera al menos la consulta SQL");
                return;
            }
            if (mdef.max_args >= 0 && argc > static_cast<size_t>(mdef.max_args)) {
                error(e.loc, "'" + obj + "." + e.object->text +
                             "()' no lleva argumentos");
                return;
            }
            if (argc > 255) { error(e.loc, "demasiados argumentos"); return; }
            chunk_->has_await = true;
            chunk_->emit(Op::CallAsync, e.loc,
                         (static_cast<uint32_t>(id) << 8) | static_cast<uint32_t>(argc));
            return;
        }

        if ((obj == "sse" && route_method_ != "SSE") ||
            (obj == "ws"  && route_method_ != "WS")) {
            error(e.object->loc, "'" + obj + "' solo existe dentro de una ruta " + obj);
            return;
        }
        if (obj == "error" && route_method_ != "ERROR") {
            error(e.object->loc, "'error' solo existe dentro de un 'on error'");
            return;
        }
    }
    else if (e.object->kind == ExprKind::Ident) {
        name = e.object->text;
        id   = native_id(name);

        // Si no es builtin, puede ser una funcion de usuario.  Declarar una con
        // el nombre de un builtin ya se rechaza al construir la tabla, asi que
        // aqui no hay ambiguedad que resolver.
        if (id < 0) {
            auto it = functions_ ? functions_->find(name) : FunctionSigs::const_iterator();
            if (functions_ && it != functions_->end()) {
                const FnSig& sig = it->second;
                size_t given = 0;
                for (const auto& a : e.args) {
                    if (!a.name.empty()) {
                        error(a.loc, "una funcion de usuario no admite argumentos "
                                     "con nombre");
                        return;
                    }
                    emit_expr(*a.value);
                    ++given;
                }

                if (given < sig.required || given > sig.defaults.size()) {
                    std::string esperado = std::to_string(sig.required);
                    if (sig.defaults.size() != sig.required)
                        esperado += " a " + std::to_string(sig.defaults.size());
                    error(e.loc, "'" + name + "()' espera " + esperado +
                                 " argumento(s), pero recibe " + std::to_string(given));
                    return;
                }

                // Los que faltan se rellenan aqui con su valor por defecto: la
                // funcion recibe siempre la lista completa.
                for (size_t i = given; i < sig.defaults.size(); ++i)
                    emit_expr(*sig.defaults[i]);

                size_t argc = sig.defaults.size();
                if (argc > 255) { error(e.loc, "demasiados argumentos"); return; }
                chunk_->emit(Op::CallFunction, e.loc,
                             (static_cast<uint32_t>(sig.index) << 8) |
                             static_cast<uint32_t>(argc));
                return;
            }
            // Constructor: una llamada al nombre de una clase.
            auto ct = classes_ ? classes_->find(name) : ClassSigs::const_iterator();
            if (classes_ && ct != classes_->end()) {
                size_t argc = 0;
                for (const auto& a : e.args) {
                    if (!a.name.empty()) {
                        error(a.loc, "un constructor no admite argumentos con nombre");
                        return;
                    }
                    emit_expr(*a.value);
                    ++argc;
                }
                auto found = ct->second.ctors.find(argc);
                if (found == ct->second.ctors.end()) {
                    std::string opciones;
                    for (const auto& [n, _] : ct->second.ctors)
                        opciones += (opciones.empty() ? "" : ", ") + std::to_string(n);
                    error(e.loc, "'" + name + "' no tiene constructor de " +
                                 std::to_string(argc) + " parametro(s)" +
                                 (opciones.empty() ? "" : "; los hay de " + opciones));
                    return;
                }
                chunk_->emit(Op::CallFunction, e.loc,
                             (static_cast<uint32_t>(found->second) << 8) |
                             static_cast<uint32_t>(argc));
                return;
            }

            error(e.object->loc, "funcion desconocida: '" + name + "'");
            return;
        }
    }
    // Metodo de clase: el tipo declarado del receptor se conoce al compilar,
    // asi que se resuelve aqui y un nombre mal escrito no llega a produccion.
    else if (e.object->kind == ExprKind::Member && classes_) {
        Type recv_type = Type::unknown();
        if (e.object->object->kind == ExprKind::Ident)
            recv_type = local_type(e.object->object->text);
        else if (e.object->object->kind == ExprKind::This)
            recv_type = local_type("this");
        const std::string recv_name = recv_type.base_name();

        auto cls = recv_type.is_unknown() ? classes_->end() : classes_->find(recv_name);
        if (cls != classes_->end()) {
            auto m = cls->second.methods.find(e.object->text);
            if (m == cls->second.methods.end()) {
                // Puede ser un campo con un metodo generico encima, o un error.
                if (!cls->second.fields.empty() &&
                    std::find(cls->second.fields.begin(), cls->second.fields.end(),
                              e.object->text) == cls->second.fields.end()) {
                    error(e.object->loc, "'" + recv_name + "' no tiene un metodo '" +
                                         e.object->text + "'");
                    return;
                }
            } else {
                const FnSig& sig = m->second;
                emit_expr(*e.object->object);          // `this`
                size_t given = 0;
                for (const auto& a : e.args) {
                    if (!a.name.empty()) {
                        error(a.loc, "un metodo no admite argumentos con nombre");
                        return;
                    }
                    emit_expr(*a.value);
                    ++given;
                }
                if (given < sig.required || given > sig.defaults.size()) {
                    error(e.loc, "'" + recv_name + "." + e.object->text +
                                 "()' espera " + std::to_string(sig.required) +
                                 " argumento(s), pero recibe " + std::to_string(given));
                    return;
                }
                for (size_t i = given; i < sig.defaults.size(); ++i)
                    emit_expr(*sig.defaults[i]);

                chunk_->emit(Op::CallFunction, e.loc,
                             (static_cast<uint32_t>(sig.index) << 8) |
                             static_cast<uint32_t>(sig.defaults.size() + 1));
                return;
            }
        }
        if (!comprobar_metodo_builtin(e)) return;
        emit_method_call_dynamic(e);
        return;
    }
    else if (e.object->kind == ExprKind::Member) {
        if (!comprobar_metodo_builtin(e)) return;
        emit_method_call_dynamic(e);
        return;
    }
    else {
        error(e.loc, "de momento solo se pueden llamar builtins o metodos");
        return;
    }

    const NativeDef& def = native_at(id);

    // Un builtin que suspende obliga a esperarlo, y uno que no, no admite
    // await: asi la firma de la llamada dice siempre si el handler se puede
    // detener ahi, sin tener que mirar la tabla de builtins.
    if (def.is_async && !awaited) {
        error(e.loc, "'" + name + "()' es asincrono: hay que escribir "
                     "'await " + name + "(...)'");
        return;
    }
    if (!def.is_async && awaited) {
        error(e.loc, "'" + name + "()' no es asincrono: sobra el 'await'");
        return;
    }

    size_t positional = 0, named = 0;
    for (const auto& a : e.args) (a.name.empty() ? positional : named)++;

    // Los argumentos con nombre son las variables de plantilla: se agrupan en
    // un Dict que ocupa el ultimo hueco posicional.
    if (named > 0 && name != "render") {
        error(e.loc, "'" + name + "()' no admite argumentos con nombre");
        return;
    }

    // ── render() con nombre literal: la plantilla se compila AQUI ────────────
    //
    // El emisor tiene delante el nombre del fichero y las claves que se le
    // pasan, que es exactamente lo que hace falta.  Compilar ahora convierte
    // una errata dentro de un {{ }} en un error de `lumen --check`.
    //
    // Si el nombre no es literal —render(variable)— no hay nada que compilar
    // por adelantado y se sigue por la via antigua.
    if (name == "render") {
        if (e.args.empty() || !e.args[0].name.empty() ||
            e.args[0].value->kind != ExprKind::StringLit) {
            // El nombre tiene que estar escrito.  Si no, no hay nada que
            // compilar por adelantado, y una plantilla que solo se comprueba
            // cuando alguien pide la pagina no vale para nada: la mitad de la
            // gracia de tenerlas en Lumen Script es que sus erratas son errores de
            // compilacion.
            error(e.loc, "render() necesita el nombre de la plantilla escrito, no una "
                         "variable. Para elegir entre varias, usa un if con nombres "
                         "literales: cada rama queda comprobada al compilar");
            return;
        }
        if (!plantillas_ || !plantillas_->tabla) {
            error(e.loc, "render() no se puede usar aqui");
            return;
        }
        emitir_render_compilado(e);
        return;
    }

    for (const auto& a : e.args)
        if (a.name.empty()) emit_expr(*a.value);

    size_t argc = positional;
    if (named > 0) {
        for (const auto& a : e.args) {
            if (a.name.empty()) continue;
            chunk_->emit(Op::Const, a.loc, chunk_->add_constant(Value::str(a.name)));
            emit_expr(*a.value);
        }
        chunk_->emit(Op::MakeDict, e.loc, static_cast<uint32_t>(named));
        ++argc;
    }

    if (static_cast<int>(argc) < def.min_args ||
        (def.max_args >= 0 && static_cast<int>(argc) > def.max_args)) {
        std::string expected = std::to_string(def.min_args);
        if (def.max_args != def.min_args)
            expected += def.max_args < 0 ? " o mas"
                                         : "-" + std::to_string(def.max_args);
        error(e.loc, "'" + name + "()' espera " + expected +
                     " argumento(s), pero recibe " + std::to_string(argc));
        return;
    }
    if (argc > 255) { error(e.loc, "demasiados argumentos"); return; }

    if (def.is_async) chunk_->has_await = true;

    chunk_->emit(def.is_async ? Op::CallAsync : Op::CallNative, e.loc,
                 (static_cast<uint32_t>(id) << 8) | static_cast<uint32_t>(argc));
}

// Traduce un ExprKind al IrExprKind correspondiente. Es una funcion, no un
// static_cast: las dos enumeraciones tienen hoy el mismo orden a proposito,
// pero un cast confiaria en eso silenciosamente. Con un switch explicito, si
// alguien anade un ExprKind sin actualizar esta funcion, el compilador avisa
// (switch no exhaustivo) en vez de dejar pasar un IrExprKind inventado.
static IrExprKind ir_kind_de(ExprKind k) {
    switch (k) {
        case ExprKind::StringLit: return IrExprKind::StringLit;
        case ExprKind::IntLit:    return IrExprKind::IntLit;
        case ExprKind::FloatLit:  return IrExprKind::FloatLit;
        case ExprKind::BoolLit:   return IrExprKind::BoolLit;
        case ExprKind::NullLit:   return IrExprKind::NullLit;
        case ExprKind::Ident:     return IrExprKind::Ident;
        case ExprKind::This:      return IrExprKind::This;
        case ExprKind::Member:    return IrExprKind::Member;
        case ExprKind::Index:     return IrExprKind::Index;
        case ExprKind::Call:      return IrExprKind::Call;
        case ExprKind::Unary:     return IrExprKind::Unary;
        case ExprKind::Binary:    return IrExprKind::Binary;
        case ExprKind::Ternary:   return IrExprKind::Ternary;
        case ExprKind::Await:     return IrExprKind::Await;
        case ExprKind::PreStep:   return IrExprKind::PreStep;
        case ExprKind::PostStep:  return IrExprKind::PostStep;
        case ExprKind::ListLit:   return IrExprKind::ListLit;
        case ExprKind::DictLit:   return IrExprKind::DictLit;
    }
    return IrExprKind::NullLit; // inalcanzable si el switch de arriba es exhaustivo
}

// Shadow de emit_expr (fase 1, checker en paralelo -- ver emitter.hpp): misma
// forma, mismo orden de comprobaciones y mismo texto de error, pero sin tocar
// chunk_ y sin declarar ninguna ranura (las declare_local() de emit_expr son
// temporales de codegen que un paso de solo comprobacion no necesita).
//
// Ademas de comprobar, construye el IrExpr de esta expresion. El tipo de
// cada nodo es SIEMPRE tipo_de(e), nunca algo mas preciso inventado aqui
// (seria funcionalidad nueva, no una reproduccion de lo que hay hoy) -- por
// eso `nodo()` lo fija una sola vez y cada case solo rellena su estructura.
// nullptr significa "ya se llamo a shadow.error en el sitio exacto": ningun
// nodo a medio construir se propaga hacia arriba.
IrExprPtr Emitter::check_expr(const Expr& e, DiagnosticBag& shadow) const {
    auto nodo = [&]() {
        auto r = std::make_unique<IrExpr>();
        r->kind = ir_kind_de(e.kind);
        r->loc  = e.loc;
        r->type = tipo_de(e);
        return r;
    };

    switch (e.kind) {
        case ExprKind::StringLit: { auto r = nodo(); r->text = e.text; return r; }
        case ExprKind::IntLit:    { auto r = nodo(); r->int_value = e.int_value; return r; }
        case ExprKind::FloatLit:  { auto r = nodo(); r->float_value = e.float_value; return r; }
        case ExprKind::BoolLit:   { auto r = nodo(); r->bool_value = e.bool_value; return r; }
        case ExprKind::NullLit:   return nodo();

        case ExprKind::Ident: {
            int slot = resolve_local(e.text);
            if (slot < 0) {
                if (native_id(e.text) >= 0)
                    shadow.error(e.loc, "'" + e.text + "' es un builtin: hay que llamarlo, "
                                 "no usarlo como valor");
                else
                    shadow.error(e.loc, "'" + e.text + "' no esta declarada");
                return nullptr;
            }
            auto r = nodo();
            r->text = e.text;
            r->slot = slot;
            return r;
        }

        case ExprKind::Unary: {
            IrExprPtr operando = check_expr(*e.lhs, shadow);
            if (!operando) return nullptr;
            auto r = nodo();
            r->text = e.text;
            r->lhs  = std::move(operando);
            return r;
        }

        case ExprKind::Binary: {
            if (e.text == "and" || e.text == "or") {
                IrExprPtr l = check_expr(*e.lhs, shadow);
                IrExprPtr r2 = check_expr(*e.rhs, shadow);
                if (!l || !r2) return nullptr;
                auto r = nodo();
                r->text = e.text;
                r->lhs = std::move(l);
                r->rhs = std::move(r2);
                return r;
            }
            IrExprPtr l = check_expr(*e.lhs, shadow);
            IrExprPtr r2 = check_expr(*e.rhs, shadow);
            static const std::set<std::string> ops = {
                "+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">="};
            if (!ops.count(e.text)) {
                shadow.error(e.loc, "operador no soportado: " + e.text);
                return nullptr;
            }
            if (!l || !r2) return nullptr;
            auto r = nodo();
            r->text = e.text;
            r->lhs = std::move(l);
            r->rhs = std::move(r2);
            return r;
        }

        case ExprKind::Ternary: {
            IrExprPtr cond = check_expr(*e.object, shadow);
            IrExprPtr si   = check_expr(*e.lhs, shadow);
            IrExprPtr no   = check_expr(*e.rhs, shadow);
            if (!cond || !si || !no) return nullptr;
            auto r = nodo();
            r->object = std::move(cond);
            r->lhs    = std::move(si);
            r->rhs    = std::move(no);
            return r;
        }

        case ExprKind::ListLit: {
            auto r = nodo();
            for (const auto& item : e.items) {
                IrExprPtr it = check_expr(*item, shadow);
                if (!it) return nullptr;
                r->items.push_back(std::move(it));
            }
            return r;
        }

        case ExprKind::DictLit: {
            auto r = nodo();
            for (const auto& entry : e.entries) {
                IrExprPtr k = check_expr(*entry.key, shadow);
                IrExprPtr v = check_expr(*entry.value, shadow);
                if (!k || !v) return nullptr;
                r->entries.push_back({std::move(k), std::move(v)});
            }
            return r;
        }

        case ExprKind::Index: {
            IrExprPtr obj = check_expr(*e.object, shadow);
            IrExprPtr idx = check_expr(*e.lhs, shadow);
            if (!obj || !idx) return nullptr;
            auto r = nodo();
            r->object = std::move(obj);
            r->lhs    = std::move(idx);
            return r;
        }

        case ExprKind::Member: {
            // `session.x` admite cualquier nombre: se traduce a
            // __session_get("x"). `text` ya lleva "x"; no hace falta mas.
            if (e.object->kind == ExprKind::Ident &&
                e.object->text == "session" &&
                resolve_local("session") < 0 &&
                member_native_id("session", e.text) < 0) {
                auto r = nodo();
                r->text = e.text;
                return r;
            }

            if (e.object->kind == ExprKind::Ident &&
                resolve_local(e.object->text) < 0 &&
                is_reserved_object(e.object->text)) {

                int id = member_native_id(e.object->text, e.text);
                if (id < 0) {
                    shadow.error(e.loc, "'" + e.object->text + "' no tiene un miembro '" +
                                 e.text + "'");
                    return nullptr;
                }
                if (native_at(id).min_args > 0) {
                    shadow.error(e.loc, "'" + e.object->text + "." + e.text +
                                 "' es una operacion: hay que llamarla con ()");
                    return nullptr;
                }
                if ((e.object->text == "sse" && route_method_ != "SSE") ||
                    (e.object->text == "ws"  && route_method_ != "WS")) {
                    shadow.error(e.loc, "'" + e.object->text + "' solo existe dentro de "
                                 "una ruta " + e.object->text);
                    return nullptr;
                }
                if (e.object->text == "error" && route_method_ != "ERROR") {
                    shadow.error(e.loc, "'error' solo existe dentro de un 'on error'");
                    return nullptr;
                }
                // Miembro de 0 argumentos de un objeto reservado (`sse.open`):
                // reusa call_name/call_index, ver el comentario de ir.hpp.
                auto r = nodo();
                r->text = e.text;
                r->call_name  = e.object->text;
                r->call_index = id;
                return r;
            }
            if (!check_campo(*e.object, e.text, e.loc, shadow)) return nullptr;
            IrExprPtr obj = check_expr(*e.object, shadow);
            if (!obj) return nullptr;
            auto r = nodo();
            r->object = std::move(obj);
            r->text   = e.text;
            return r;
        }

        case ExprKind::Call:
            return check_call(e, /*awaited=*/false, shadow);

        case ExprKind::PreStep:
        case ExprKind::PostStep: {
            const Expr& tgt = *e.lhs;

            if (tgt.kind == ExprKind::Ident) {
                int slot = resolve_local(tgt.text);
                if (slot < 0) {
                    shadow.error(tgt.loc, "'" + tgt.text + "' no esta declarada");
                    return nullptr;
                }
                auto objetivo = std::make_unique<IrExpr>();
                objetivo->kind = IrExprKind::Ident;
                objetivo->loc  = tgt.loc;
                objetivo->text = tgt.text;
                objetivo->slot = slot;
                objetivo->type = local_type(tgt.text);
                auto r = nodo();
                r->text = e.text;
                r->lhs  = std::move(objetivo);
                return r;
            }
            if (tgt.kind == ExprKind::Member) {
                // Igual que emit_expr: NO se comprueba que el campo exista
                // (comprobar_campo no se llama aqui hoy tampoco). Reproducir
                // ese hueco, no arreglarlo, es lo que toca en esta fase.
                IrExprPtr obj = check_expr(*tgt.object, shadow);
                if (!obj) return nullptr;
                auto objetivo = std::make_unique<IrExpr>();
                objetivo->kind   = IrExprKind::Member;
                objetivo->loc    = tgt.loc;
                objetivo->text   = tgt.text;
                objetivo->object = std::move(obj);
                auto r = nodo();
                r->text = e.text;
                r->lhs  = std::move(objetivo);
                return r;
            }
            if (tgt.kind == ExprKind::Index) {
                IrExprPtr obj = check_expr(*tgt.object, shadow);
                IrExprPtr idx = check_expr(*tgt.lhs, shadow);
                if (!obj || !idx) return nullptr;
                auto objetivo = std::make_unique<IrExpr>();
                objetivo->kind   = IrExprKind::Index;
                objetivo->loc    = tgt.loc;
                objetivo->object = std::move(obj);
                objetivo->lhs    = std::move(idx);
                auto r = nodo();
                r->text = e.text;
                r->lhs  = std::move(objetivo);
                return r;
            }
            shadow.error(e.loc, "'++' y '--' solo se aplican a una variable, un campo "
                         "o un elemento indexado");
            return nullptr;
        }

        case ExprKind::Await: {
            if (!e.lhs || e.lhs->kind != ExprKind::Call) {
                shadow.error(e.loc, "'await' solo se aplica a una llamada asincrona "
                             "(de momento: sleep)");
                return nullptr;
            }
            IrExprPtr llamada = check_call(*e.lhs, /*awaited=*/true, shadow);
            if (!llamada) return nullptr;
            auto r = nodo();
            r->lhs = std::move(llamada);
            return r;
        }

        case ExprKind::This: {
            int slot = resolve_local("this");
            if (slot < 0) {
                shadow.error(e.loc, "'this' solo existe dentro de un metodo o un constructor");
                return nullptr;
            }
            auto r = nodo();
            r->slot = slot;
            return r;
        }
    }
    return nullptr; // inalcanzable si el switch de arriba es exhaustivo
}

// Shadow de emit_call: misma forma, mismo orden, mismo texto -- ver el
// comentario de check_expr. Construye un IrExpr(kind=Call) con call_shape ya
// resuelto a una de las 8 formas de COMPILACION-NATIVA.md §1.2.
IrExprPtr Emitter::check_call(const Expr& e, bool awaited, DiagnosticBag& shadow) const {
    if (!e.object) { shadow.error(e.loc, "llamada sin destino"); return nullptr; }

    auto llamada = [&](IrCallShape shape) {
        auto r = std::make_unique<IrExpr>();
        r->kind = IrExprKind::Call;
        r->loc  = e.loc;
        r->type = tipo_de(e);
        r->call_shape = shape;
        r->awaited    = awaited;
        return r;
    };

    std::string  name;
    int          id = -1;
    IrCallShape  shape = IrCallShape::Invalid;

    // sse.send(...) — miembro de un objeto reservado.
    if (e.object->kind == ExprKind::Member &&
        e.object->object->kind == ExprKind::Ident &&
        resolve_local(e.object->object->text) < 0 &&
        is_reserved_object(e.object->object->text)) {

        name = e.object->object->text + "." + e.object->text;
        id   = member_native_id(e.object->object->text, e.object->text);
        if (id < 0) {
            shadow.error(e.object->loc, "'" + e.object->object->text +
                                 "' no tiene un miembro '" + e.object->text + "'");
            return nullptr;
        }
        const std::string& obj = e.object->object->text;

        bool is_module = is_db_module(obj);
        if (is_module && (!imports_ || !imports_->count(obj))) {
            shadow.error(e.object->loc, "falta 'import " + obj + "' para poder usar '" +
                                 obj + "." + e.object->text + "'");
            return nullptr;
        }
        if (is_module) {
            const NativeDef& mdef = native_at(id);
            if (!awaited) {
                shadow.error(e.loc, "'" + obj + "." + e.object->text + "()' es asincrono: "
                             "hay que escribir 'await " + obj + "." + e.object->text +
                             "(...)'");
                return nullptr;
            }
            IrExprPtr r = llamada(IrCallShape::DbModuleCall);
            r->call_name  = obj;
            r->call_index = id;

            size_t argc = 1;
            for (const auto& a : e.args) {
                if (!a.name.empty()) {
                    shadow.error(a.loc, "las consultas no admiten argumentos con nombre");
                    return nullptr;
                }
                IrExprPtr v = check_expr(*a.value, shadow);
                if (!v) return nullptr;
                r->args.push_back({{}, std::move(v), a.loc});
                ++argc;
            }
            if (argc < static_cast<size_t>(mdef.min_args)) {
                shadow.error(e.loc, "'" + obj + "." + e.object->text +
                             "()' espera al menos la consulta SQL");
                return nullptr;
            }
            if (mdef.max_args >= 0 && argc > static_cast<size_t>(mdef.max_args)) {
                shadow.error(e.loc, "'" + obj + "." + e.object->text +
                             "()' no lleva argumentos");
                return nullptr;
            }
            if (argc > 255) { shadow.error(e.loc, "demasiados argumentos"); return nullptr; }
            return r;
        }

        if ((obj == "sse" && route_method_ != "SSE") ||
            (obj == "ws"  && route_method_ != "WS")) {
            shadow.error(e.object->loc, "'" + obj + "' solo existe dentro de una ruta " + obj);
            return nullptr;
        }
        if (obj == "error" && route_method_ != "ERROR") {
            shadow.error(e.object->loc, "'error' solo existe dentro de un 'on error'");
            return nullptr;
        }
        shape = IrCallShape::ReservedMemberCall;
    }
    else if (e.object->kind == ExprKind::Ident) {
        name = e.object->text;
        id   = native_id(name);

        if (id < 0) {
            auto it = functions_ ? functions_->find(name) : FunctionSigs::const_iterator();
            if (functions_ && it != functions_->end()) {
                const FnSig& sig = it->second;
                IrExprPtr r = llamada(IrCallShape::UserFunctionCall);
                r->call_index = static_cast<int>(sig.index);

                size_t given = 0;
                for (const auto& a : e.args) {
                    if (!a.name.empty()) {
                        shadow.error(a.loc, "una funcion de usuario no admite argumentos "
                                     "con nombre");
                        return nullptr;
                    }
                    IrExprPtr v = check_expr(*a.value, shadow);
                    if (!v) return nullptr;
                    r->args.push_back({{}, std::move(v), a.loc});
                    ++given;
                }

                if (given < sig.required || given > sig.defaults.size()) {
                    std::string esperado = std::to_string(sig.required);
                    if (sig.defaults.size() != sig.required)
                        esperado += " a " + std::to_string(sig.defaults.size());
                    shadow.error(e.loc, "'" + name + "()' espera " + esperado +
                                 " argumento(s), pero recibe " + std::to_string(given));
                    return nullptr;
                }
                // Los que faltan se rellenan con su valor por defecto: la
                // funcion recibe siempre la lista completa (igual que hoy
                // emit_call, ver el comentario de IrArg).
                for (size_t i = given; i < sig.defaults.size(); ++i) {
                    IrExprPtr v = check_expr(*sig.defaults[i], shadow);
                    if (!v) return nullptr;
                    r->args.push_back({{}, std::move(v), e.loc});
                }
                return r;
            }
            auto ct = classes_ ? classes_->find(name) : ClassSigs::const_iterator();
            if (classes_ && ct != classes_->end()) {
                IrExprPtr r = llamada(IrCallShape::ConstructorCall);

                size_t argc = 0;
                for (const auto& a : e.args) {
                    if (!a.name.empty()) {
                        shadow.error(a.loc, "un constructor no admite argumentos con nombre");
                        return nullptr;
                    }
                    IrExprPtr v = check_expr(*a.value, shadow);
                    if (!v) return nullptr;
                    r->args.push_back({{}, std::move(v), a.loc});
                    ++argc;
                }
                auto found = ct->second.ctors.find(argc);
                if (found == ct->second.ctors.end()) {
                    std::string opciones;
                    for (const auto& [n, _] : ct->second.ctors)
                        opciones += (opciones.empty() ? "" : ", ") + std::to_string(n);
                    shadow.error(e.loc, "'" + name + "' no tiene constructor de " +
                                 std::to_string(argc) + " parametro(s)" +
                                 (opciones.empty() ? "" : "; los hay de " + opciones));
                    return nullptr;
                }
                r->call_index = static_cast<int>(found->second);
                return r;
            }

            shadow.error(e.object->loc, "funcion desconocida: '" + name + "'");
            return nullptr;
        }
        shape = IrCallShape::BuiltinGlobalCall;
    }
    else if (e.object->kind == ExprKind::Member && classes_) {
        Type recv_type = Type::unknown();
        if (e.object->object->kind == ExprKind::Ident)
            recv_type = local_type(e.object->object->text);
        else if (e.object->object->kind == ExprKind::This)
            recv_type = local_type("this");
        const std::string recv_name = recv_type.base_name();

        auto cls = recv_type.is_unknown() ? classes_->end() : classes_->find(recv_name);
        if (cls != classes_->end()) {
            auto m = cls->second.methods.find(e.object->text);
            if (m == cls->second.methods.end()) {
                if (!cls->second.fields.empty() &&
                    std::find(cls->second.fields.begin(), cls->second.fields.end(),
                              e.object->text) == cls->second.fields.end()) {
                    shadow.error(e.object->loc, "'" + recv_name + "' no tiene un metodo '" +
                                         e.object->text + "'");
                    return nullptr;
                }
                // Campo con un metodo generico encima: sigue mas abajo.
            } else {
                const FnSig& sig = m->second;
                IrExprPtr receptor = check_expr(*e.object->object, shadow);
                if (!receptor) return nullptr;

                IrExprPtr r = llamada(IrCallShape::ClassMethodCall);
                r->object     = std::move(receptor);
                r->call_index = static_cast<int>(sig.index);

                size_t given = 0;
                for (const auto& a : e.args) {
                    if (!a.name.empty()) {
                        shadow.error(a.loc, "un metodo no admite argumentos con nombre");
                        return nullptr;
                    }
                    IrExprPtr v = check_expr(*a.value, shadow);
                    if (!v) return nullptr;
                    r->args.push_back({{}, std::move(v), a.loc});
                    ++given;
                }
                if (given < sig.required || given > sig.defaults.size()) {
                    shadow.error(e.loc, "'" + recv_name + "." + e.object->text +
                                 "()' espera " + std::to_string(sig.required) +
                                 " argumento(s), pero recibe " + std::to_string(given));
                    return nullptr;
                }
                for (size_t i = given; i < sig.defaults.size(); ++i) {
                    IrExprPtr v = check_expr(*sig.defaults[i], shadow);
                    if (!v) return nullptr;
                    r->args.push_back({{}, std::move(v), e.loc});
                }
                return r;
            }
        }
        if (!check_metodo_builtin(e, shadow)) return nullptr;
        IrExprPtr receptor = check_expr(*e.object->object, shadow);
        if (!receptor) return nullptr;
        IrExprPtr r = llamada(IrCallShape::BuiltinMethodCall);
        r->object    = std::move(receptor);
        r->call_name = e.object->text;
        for (const auto& a : e.args) {
            IrExprPtr v = check_expr(*a.value, shadow);
            if (!v) return nullptr;
            // A diferencia de las demas formas, un metodo builtin SI admite
            // nombrados (comprobar_metodo_builtin los cuenta como un hueco
            // mas, no los rechaza): a.name viaja, para que emit_method_call_
            // dynamic pueda agruparlos en el Dict del ultimo hueco
            // posicional, igual que hace hoy con el Expr original.
            r->args.push_back({a.name, std::move(v), a.loc});
        }
        return r;
    }
    else if (e.object->kind == ExprKind::Member) {
        if (!check_metodo_builtin(e, shadow)) return nullptr;
        IrExprPtr receptor = check_expr(*e.object->object, shadow);
        if (!receptor) return nullptr;
        IrExprPtr r = llamada(IrCallShape::BuiltinMethodCall);
        r->object    = std::move(receptor);
        r->call_name = e.object->text;
        for (const auto& a : e.args) {
            IrExprPtr v = check_expr(*a.value, shadow);
            if (!v) return nullptr;
            // A diferencia de las demas formas, un metodo builtin SI admite
            // nombrados (comprobar_metodo_builtin los cuenta como un hueco
            // mas, no los rechaza): a.name viaja, para que emit_method_call_
            // dynamic pueda agruparlos en el Dict del ultimo hueco
            // posicional, igual que hace hoy con el Expr original.
            r->args.push_back({a.name, std::move(v), a.loc});
        }
        return r;
    }
    else {
        shadow.error(e.loc, "de momento solo se pueden llamar builtins o metodos");
        return nullptr;
    }

    // Cola compartida por las formas 2 (ReservedMemberCall) y 6
    // (BuiltinGlobalCall): las dos resuelven a un native_id valido y
    // comparten exactamente las mismas reglas de await/nombrados/aridad.
    const NativeDef& def = native_at(id);

    if (def.is_async && !awaited) {
        shadow.error(e.loc, "'" + name + "()' es asincrono: hay que escribir "
                     "'await " + name + "(...)'");
        return nullptr;
    }
    if (!def.is_async && awaited) {
        shadow.error(e.loc, "'" + name + "()' no es asincrono: sobra el 'await'");
        return nullptr;
    }

    size_t positional = 0, named = 0;
    for (const auto& a : e.args) (a.name.empty() ? positional : named)++;

    if (named > 0 && name != "render") {
        shadow.error(e.loc, "'" + name + "()' no admite argumentos con nombre");
        return nullptr;
    }

    IrExprPtr r = llamada(shape);
    r->call_name  = name;
    r->call_index = id;

    // render() compila la plantilla en si en emitir_render_compilado, que es
    // superficie de plantillas (fase 3), no de expresiones -- no se reproduce
    // aqui; solo se valida lo que ya se valido arriba mas las subexpresiones.
    if (name == "render") {
        if (e.args.empty() || !e.args[0].name.empty() ||
            e.args[0].value->kind != ExprKind::StringLit) {
            shadow.error(e.loc, "render() necesita el nombre de la plantilla escrito, no una "
                         "variable. Para elegir entre varias, usa un if con nombres "
                         "literales: cada rama queda comprobada al compilar");
            return nullptr;
        }
        if (!plantillas_ || !plantillas_->tabla) {
            shadow.error(e.loc, "render() no se puede usar aqui");
            return nullptr;
        }
        for (const auto& a : e.args) {
            IrExprPtr v = check_expr(*a.value, shadow);
            if (!v) return nullptr;
            r->args.push_back({a.name, std::move(v), a.loc});
        }
        return r;
    }

    for (const auto& a : e.args) {
        if (!a.name.empty()) continue;
        IrExprPtr v = check_expr(*a.value, shadow);
        if (!v) return nullptr;
        r->args.push_back({{}, std::move(v), a.loc});
    }

    size_t argc = positional;
    if (named > 0) {
        for (const auto& a : e.args) {
            if (a.name.empty()) continue;
            IrExprPtr v = check_expr(*a.value, shadow);
            if (!v) return nullptr;
            r->args.push_back({a.name, std::move(v), a.loc});
        }
        ++argc;
    }

    if (static_cast<int>(argc) < def.min_args ||
        (def.max_args >= 0 && static_cast<int>(argc) > def.max_args)) {
        std::string expected = std::to_string(def.min_args);
        if (def.max_args != def.min_args)
            expected += def.max_args < 0 ? " o mas"
                                         : "-" + std::to_string(def.max_args);
        shadow.error(e.loc, "'" + name + "()' espera " + expected +
                     " argumento(s), pero recibe " + std::to_string(argc));
        return nullptr;
    }
    if (argc > 255) { shadow.error(e.loc, "demasiados argumentos"); return nullptr; }

    return r;
}

// Compila la plantilla contra las claves de esta llamada y emite una llamada a
// __render_tpl(indice, datos).
void Emitter::emitir_render_compilado(const Expr& e) {
    const std::string& nombre = e.args[0].value->text;

    if (nombre.find("..") != std::string::npos ||
        std::filesystem::path(nombre).is_absolute()) {
        error(e.args[0].loc, "nombre de plantilla no valido: '" + nombre + "'");
        return;
    }

    // Cada clave viaja con el tipo de lo que se le pasa: es lo que permite que
    // dentro de la plantilla `{{ titulo.mayusculas() }}` sea un error aqui y no
    // una pagina rota en produccion.
    std::vector<NombreTipado> claves;
    for (const auto& a : e.args) {
        if (a.name.empty()) continue;
        claves.push_back({a.name, tipo_de(*a.value).base_name()});
    }

    const std::filesystem::path ruta =
        std::filesystem::path(plantillas_->dir) / nombre;
    std::ifstream f(ruta, std::ios::binary);
    if (!f) {
        error(e.args[0].loc, "no se encuentra la plantilla '" + nombre + "' en " +
                             plantillas_->dir);
        return;
    }
    const std::string fuente((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

    Plantilla tpl;
    if (!compilar_plantilla(fuente, nombre, plantillas_->dir, claves, diags_, tpl)) {
        failed_ = true;
        return;
    }
    const uint32_t idx = static_cast<uint32_t>(plantillas_->tabla->size());
    plantillas_->tabla->push_back(std::move(tpl));

    // __render_tpl(indice, datos)
    chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::integer(idx)));
    for (const auto& a : e.args) {
        if (a.name.empty()) continue;
        chunk_->emit(Op::Const, a.loc, chunk_->add_constant(Value::str(a.name)));
        emit_expr(*a.value);
    }
    chunk_->emit(Op::MakeDict, e.loc, static_cast<uint32_t>(claves.size()));

    const int id = native_id("__render_tpl");
    chunk_->emit(Op::CallNative, e.loc, (static_cast<uint32_t>(id) << 8) | 2u);
}

// ═══════════════════════════════════════════════════════════════════════════
// Fase 1 (COMPILACION-NATIVA.md): emisor que consume el IR, todavia sin
// conectar a ningun punto de entrada real. Ver el comentario de
// emitter.hpp junto a estas declaraciones para el porque de la sobrecarga
// de nombre. Cada funcion de aqui es la contrapartida MECANICA de su
// version sobre Expr/Stmt: mismos opcodes, mismo orden -- la unica
// diferencia es que lee campos ya resueltos (slot, call_shape, call_index,
// call_name, type) en vez de resolverlos, y no llama a error() en ningun
// sitio porque check_expr/check_stmt ya lo hicieron antes de construir
// este nodo.
// ═══════════════════════════════════════════════════════════════════════════

bool Emitter::es_int_ir(const IrExpr& e) const {
    switch (e.kind) {
        case IrExprKind::IntLit: return true;
        case IrExprKind::Ident:  return e.type.base_name() == "int";
        case IrExprKind::Binary:
            if (e.text == "+" || e.text == "-" || e.text == "*")
                return e.lhs && e.rhs && es_int_ir(*e.lhs) && es_int_ir(*e.rhs);
            return false;
        default: return false;
    }
}

void Emitter::emit_expr(const IrExpr& e) {
    switch (e.kind) {
        case IrExprKind::StringLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::str(e.text)));
            break;
        case IrExprKind::IntLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::integer(e.int_value)));
            break;
        case IrExprKind::FloatLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::real(e.float_value)));
            break;
        case IrExprKind::BoolLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::boolean(e.bool_value)));
            break;
        case IrExprKind::NullLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::null()));
            break;

        case IrExprKind::Ident:
        case IrExprKind::This:
            chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(e.slot));
            break;

        case IrExprKind::Unary:
            emit_expr(*e.lhs);
            chunk_->emit(e.text == "not" ? Op::Not : Op::Neg, e.loc);
            break;

        case IrExprKind::Binary: {
            if (e.text == "and" || e.text == "or") {
                emit_expr(*e.lhs);
                size_t j = chunk_->emit(e.text == "and" ? Op::JumpIfFalsePeek
                                                        : Op::JumpIfTruePeek, e.loc);
                emit_expr(*e.rhs);
                chunk_->patch(j, chunk_->here());
                break;
            }

            emit_expr(*e.lhs);
            emit_expr(*e.rhs);
            Op op = Op::Add;
            if      (e.text == "+")  op = Op::Add;
            else if (e.text == "-")  op = Op::Sub;
            else if (e.text == "*")  op = Op::Mul;
            else if (e.text == "/")  op = Op::Div;
            else if (e.text == "%")  op = Op::Mod;
            else if (e.text == "==") op = Op::Eq;
            else if (e.text == "!=") op = Op::Ne;
            else if (e.text == "<")  op = Op::Lt;
            else if (e.text == "<=") op = Op::Le;
            else if (e.text == ">")  op = Op::Gt;
            else if (e.text == ">=") op = Op::Ge;
            // check_expr ya rechazo cualquier operador que no sea uno de
            // estos once -- un IrExpr::Binary con otro texto no deberia
            // poder existir.

            if (es_int_ir(*e.lhs) && es_int_ir(*e.rhs)) {
                switch (op) {
                    case Op::Add: op = Op::AddInt; break;
                    case Op::Sub: op = Op::SubInt; break;
                    case Op::Mul: op = Op::MulInt; break;
                    case Op::Lt:  op = Op::LtInt;  break;
                    case Op::Le:  op = Op::LeInt;  break;
                    case Op::Gt:  op = Op::GtInt;  break;
                    case Op::Ge:  op = Op::GeInt;  break;
                    default: break;
                }
            }
            chunk_->emit(op, e.loc);
            break;
        }

        case IrExprKind::Ternary: {
            emit_expr(*e.object);
            size_t to_else = chunk_->emit(Op::JumpIfFalse, e.loc);
            emit_expr(*e.lhs);
            size_t to_end = chunk_->emit(Op::Jump, e.loc);
            chunk_->patch(to_else, chunk_->here());
            emit_expr(*e.rhs);
            chunk_->patch(to_end, chunk_->here());
            break;
        }

        case IrExprKind::ListLit:
            for (const auto& item : e.items) emit_expr(*item);
            chunk_->emit(Op::MakeList, e.loc, static_cast<uint32_t>(e.items.size()));
            break;

        case IrExprKind::DictLit:
            for (const auto& entry : e.entries) {
                emit_expr(*entry.key);
                emit_expr(*entry.value);
            }
            chunk_->emit(Op::MakeDict, e.loc, static_cast<uint32_t>(e.entries.size()));
            break;

        case IrExprKind::Index:
            emit_expr(*e.object);
            emit_expr(*e.lhs);
            chunk_->emit(Op::GetIndex, e.loc);
            break;

        case IrExprKind::Member: {
            // Sin receptor: o bien session.x (__session_get) o bien un
            // miembro de 0 argumentos de un objeto reservado (sse.open),
            // distinguibles por si call_name viene relleno -- ver el
            // comentario de ir.hpp sobre este reuso.
            if (!e.object) {
                if (e.call_name.empty()) {
                    chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::str(e.text)));
                    chunk_->emit(Op::CallNative, e.loc,
                                 (static_cast<uint32_t>(native_id("__session_get")) << 8) | 1u);
                } else {
                    chunk_->emit(Op::CallNative, e.loc, static_cast<uint32_t>(e.call_index) << 8);
                }
                break;
            }
            emit_expr(*e.object);
            chunk_->emit(Op::GetMember, e.loc, chunk_->add_constant(Value::str(e.text)));
            break;
        }

        case IrExprKind::Call:
            emit_call(e);
            break;

        case IrExprKind::PreStep:
        case IrExprKind::PostStep: {
            bool post = (e.kind == IrExprKind::PostStep);
            Op   op   = (e.text == "+") ? Op::Add : Op::Sub;
            const IrExpr& tgt = *e.lhs;

            auto one = [&] {
                chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::integer(1)));
            };

            if (tgt.kind == IrExprKind::Ident) {
                uint32_t u = static_cast<uint32_t>(tgt.slot);

                if (post) chunk_->emit(Op::LoadLocal, e.loc, u);
                chunk_->emit(Op::LoadLocal, e.loc, u);
                one();
                chunk_->emit(op, e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, u);
                if (!post) chunk_->emit(Op::LoadLocal, e.loc, u);
                break;
            }

            if (tgt.kind == IrExprKind::Member) {
                begin_scope();
                uint32_t name_k = chunk_->add_constant(Value::str(tgt.text));

                emit_expr(*tgt.object);
                int obj = declare_local(" recep", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(obj));

                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(obj));
                chunk_->emit(Op::GetMember, e.loc, name_k);
                int prev = declare_local(" previo", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(prev));

                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(obj));
                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(prev));
                one();
                chunk_->emit(op, e.loc);
                chunk_->emit(Op::SetMember, e.loc, name_k);

                if (post) {
                    chunk_->emit(Op::Pop, e.loc);
                    chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(prev));
                }
                end_scope();
                break;
            }

            if (tgt.kind == IrExprKind::Index) {
                begin_scope();
                emit_expr(*tgt.object);
                int cont = declare_local(" contened", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(cont));

                emit_expr(*tgt.lhs);
                int idx = declare_local(" indice", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(idx));

                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(cont));
                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(idx));
                chunk_->emit(Op::GetIndex, e.loc);
                int prev = declare_local(" previo", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(prev));

                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(cont));
                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(idx));
                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(prev));
                one();
                chunk_->emit(op, e.loc);
                chunk_->emit(Op::SetIndex, e.loc);

                if (post) {
                    chunk_->emit(Op::Pop, e.loc);
                    chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(prev));
                }
                end_scope();
                break;
            }
            // check_expr ya rechazo cualquier otro objetivo de ++/--.
            break;
        }

        case IrExprKind::Await:
            emit_call(*e.lhs); // e.lhs es siempre Call, con awaited ya en true
            break;
    }
}

// Contrapartida de emit_method_call_dynamic sobre el IR: mismo orden
// (receptor, posicionales, nombrados agrupados en un Dict), mismo opcode.
void Emitter::emit_method_call_dynamic(const IrExpr& e) {
    emit_expr(*e.object);
    uint32_t argc = 0, named = 0;
    for (const auto& a : e.args) {
        if (!a.name.empty()) { ++named; continue; }
        emit_expr(*a.value);
        ++argc;
    }
    if (named > 0) {
        for (const auto& a : e.args) {
            if (a.name.empty()) continue;
            chunk_->emit(Op::Const, a.loc, chunk_->add_constant(Value::str(a.name)));
            emit_expr(*a.value);
        }
        chunk_->emit(Op::MakeDict, e.loc, named);
        ++argc;
    }
    // check_metodo_builtin ya rechazo mas de 255 argumentos.
    chunk_->emit(Op::CallMethod, e.loc,
                 (chunk_->add_constant(Value::str(e.call_name)) << 8) | argc);
}

// Cola compartida por ReservedMemberCall y BuiltinGlobalCall (salvo
// render()): las dos resuelven a un native_id valido con las mismas reglas
// de nombrados que hoy tiene emit_call.
void Emitter::emit_native_call(const IrExpr& e) {
    uint32_t argc = 0, named = 0;
    for (const auto& a : e.args) {
        if (a.name.empty()) { emit_expr(*a.value); ++argc; }
        else ++named;
    }
    if (named > 0) {
        for (const auto& a : e.args) {
            if (a.name.empty()) continue;
            chunk_->emit(Op::Const, a.loc, chunk_->add_constant(Value::str(a.name)));
            emit_expr(*a.value);
        }
        chunk_->emit(Op::MakeDict, e.loc, named);
        ++argc;
    }
    const NativeDef& def = native_at(e.call_index);
    if (def.is_async) chunk_->has_await = true;
    chunk_->emit(def.is_async ? Op::CallAsync : Op::CallNative, e.loc,
                 (static_cast<uint32_t>(e.call_index) << 8) | argc);
}

void Emitter::emit_call(const IrExpr& e) {
    switch (e.call_shape) {
        case IrCallShape::DbModuleCall: {
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::str(e.call_name)));
            for (const auto& a : e.args) emit_expr(*a.value);
            chunk_->has_await = true;
            chunk_->emit(Op::CallAsync, e.loc,
                         (static_cast<uint32_t>(e.call_index) << 8) |
                         static_cast<uint32_t>(e.args.size() + 1));
            break;
        }

        case IrCallShape::UserFunctionCall:
        case IrCallShape::ConstructorCall: {
            for (const auto& a : e.args) emit_expr(*a.value);
            chunk_->emit(Op::CallFunction, e.loc,
                         (static_cast<uint32_t>(e.call_index) << 8) |
                         static_cast<uint32_t>(e.args.size()));
            break;
        }

        case IrCallShape::ClassMethodCall: {
            emit_expr(*e.object); // `this`
            for (const auto& a : e.args) emit_expr(*a.value);
            chunk_->emit(Op::CallFunction, e.loc,
                         (static_cast<uint32_t>(e.call_index) << 8) |
                         static_cast<uint32_t>(e.args.size() + 1));
            break;
        }

        case IrCallShape::ReservedMemberCall:
            emit_native_call(e);
            break;

        case IrCallShape::BuiltinGlobalCall:
            if (e.call_name == "render") emitir_render_compilado(e);
            else                         emit_native_call(e);
            break;

        case IrCallShape::BuiltinMethodCall:
            emit_method_call_dynamic(e);
            break;

        case IrCallShape::Invalid:
            break; // inalcanzable: check_call ya lo rechazo
    }
}

// Contrapartida de emitir_render_compilado sobre el IR. Sigue siendo la
// unica excepcion real a "check_* es la unica fuente de diagnosticos": la
// plantilla en si (su fichero, sus propios {{ }}) no se puede validar sin
// leerla y compilarla, y eso es superficie de plantillas (fase 3), no algo
// que check_call pueda anticipar sobre una expresion. error()/diags_ siguen
// siendo correctos aqui a proposito.
void Emitter::emitir_render_compilado(const IrExpr& e) {
    const std::string& nombre = e.args[0].value->text;

    if (nombre.find("..") != std::string::npos ||
        std::filesystem::path(nombre).is_absolute()) {
        error(e.args[0].loc, "nombre de plantilla no valido: '" + nombre + "'");
        return;
    }

    std::vector<NombreTipado> claves;
    for (const auto& a : e.args) {
        if (a.name.empty()) continue;
        claves.push_back({a.name, a.value->type.base_name()});
    }

    const std::filesystem::path ruta =
        std::filesystem::path(plantillas_->dir) / nombre;
    std::ifstream f(ruta, std::ios::binary);
    if (!f) {
        error(e.args[0].loc, "no se encuentra la plantilla '" + nombre + "' en " +
                             plantillas_->dir);
        return;
    }
    const std::string fuente((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

    Plantilla tpl;
    if (!compilar_plantilla(fuente, nombre, plantillas_->dir, claves, diags_, tpl)) {
        failed_ = true;
        return;
    }
    const uint32_t idx = static_cast<uint32_t>(plantillas_->tabla->size());
    plantillas_->tabla->push_back(std::move(tpl));

    chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::integer(idx)));
    for (const auto& a : e.args) {
        if (a.name.empty()) continue;
        chunk_->emit(Op::Const, a.loc, chunk_->add_constant(Value::str(a.name)));
        emit_expr(*a.value);
    }
    chunk_->emit(Op::MakeDict, e.loc, static_cast<uint32_t>(claves.size()));

    const int id = native_id("__render_tpl");
    chunk_->emit(Op::CallNative, e.loc, (static_cast<uint32_t>(id) << 8) | 2u);
}

void Emitter::emit_block(const IrBlock& body) {
    begin_scope();
    for (const auto& s : body) emit_stmt(*s);
    end_scope();
}

void Emitter::emit_stmt(const IrStmt& s) {
    switch (s.kind) {
        case IrStmtKind::Return:
            if (s.value) { emit_expr(*s.value); chunk_->emit(Op::Return, s.loc); }
            else         { chunk_->emit(Op::ReturnNull, s.loc); }
            break;

        case IrStmtKind::ExprStmt:
            emit_expr(*s.value);
            chunk_->emit(Op::Pop, s.loc);
            break;

        case IrStmtKind::VarDecl:
            if (s.value) emit_expr(*s.value);
            else         chunk_->emit(Op::Const, s.loc, chunk_->add_constant(Value::null()));
            // declare_local() hay que llamarlo aqui tambien, aunque el slot
            // real ya lo trae s.slot: es lo que mantiene locals_ (y por
            // tanto el proximo slot que calcule un declare_local posterior
            // -- el `for` desazucarado, un `catch`, un temporal de ++/--)
            // sincronizado con el que ya calculo check_stmt. Sin esto, el
            // primer VarDecl del cuerpo deja locals_ mas corto de lo que
            // deberia y todo lo que se declare despues cae en la ranura
            // equivocada -- exactamente el bug que encontro
            // tests/emit_ir_shadow.cpp antes de este arreglo.
            declare_local(s.name, s.loc, s.decl_type);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(s.slot));
            break;

        case IrStmtKind::Assign: {
            switch (s.assign_target) {
                case IrAssignTarget::Session:
                    chunk_->emit(Op::Const, s.loc,
                                 chunk_->add_constant(Value::str(s.assign_field)));
                    emit_expr(*s.value);
                    chunk_->emit(Op::CallNative, s.loc,
                                 (static_cast<uint32_t>(native_id("__session_set")) << 8) | 2u);
                    chunk_->emit(Op::Pop, s.loc);
                    break;

                case IrAssignTarget::Index:
                    emit_expr(*s.assign_object);
                    emit_expr(*s.assign_index);
                    emit_expr(*s.value);
                    chunk_->emit(Op::SetIndex, s.loc);
                    chunk_->emit(Op::Pop, s.loc);
                    break;

                case IrAssignTarget::Member:
                    emit_expr(*s.assign_object);
                    emit_expr(*s.value);
                    chunk_->emit(Op::SetMember, s.loc,
                                 chunk_->add_constant(Value::str(s.assign_field)));
                    chunk_->emit(Op::Pop, s.loc);
                    break;

                case IrAssignTarget::Local:
                    emit_expr(*s.value);
                    chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(s.assign_slot));
                    break;
            }
            break;
        }

        case IrStmtKind::If: {
            emit_expr(*s.value);
            size_t to_else = chunk_->emit(Op::JumpIfFalse, s.loc);
            emit_block(s.body);

            if (!s.orelse.empty()) {
                size_t to_end = chunk_->emit(Op::Jump, s.loc);
                chunk_->patch(to_else, chunk_->here());
                emit_block(s.orelse);
                chunk_->patch(to_end, chunk_->here());
            } else {
                chunk_->patch(to_else, chunk_->here());
            }
            break;
        }

        case IrStmtKind::While: {
            size_t start = chunk_->here();
            emit_expr(*s.value);
            size_t to_end = chunk_->emit(Op::JumpIfFalse, s.loc);

            loops_.push_back({});
            emit_block(s.body);
            for (size_t j : loops_.back().continues) chunk_->patch(j, start);
            chunk_->emit(Op::Jump, s.loc, static_cast<uint32_t>(start));
            chunk_->patch(to_end, chunk_->here());

            for (size_t j : loops_.back().breaks) chunk_->patch(j, chunk_->here());
            loops_.pop_back();
            break;
        }

        case IrStmtKind::Require: {
            emit_expr(*s.value);
            size_t to_ok = chunk_->emit(Op::JumpIfFalse, s.loc);
            size_t skip  = chunk_->emit(Op::Jump, s.loc);
            chunk_->patch(to_ok, chunk_->here());
            emit_expr(*s.target);
            chunk_->emit(Op::Return, s.loc);
            chunk_->patch(skip, chunk_->here());
            break;
        }

        case IrStmtKind::Break:
            loops_.back().breaks.push_back(chunk_->emit(Op::Jump, s.loc));
            break;

        case IrStmtKind::Continue:
            loops_.back().continues.push_back(chunk_->emit(Op::Jump, s.loc));
            break;

        case IrStmtKind::For: {
            begin_scope();

            emit_expr(*s.target);
            chunk_->emit(Op::IterList, s.loc);
            int items = declare_local(" items", s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(items));

            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(items));
            chunk_->emit(Op::CallNative, s.loc,
                         (static_cast<uint32_t>(native_id("len")) << 8) | 1u);
            int count = declare_local(" count", s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(count));

            chunk_->emit(Op::Const, s.loc, chunk_->add_constant(Value::integer(0)));
            int index = declare_local(" index", s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(index));

            // `s.slot` ya es la ranura de la variable del bucle -- pero
            // declare_local hay que llamarlo igual, en el mismo orden, para
            // que items/count/index caigan en las ranuras que check_stmt ya
            // calculo (ver el comentario de IrStmt::slot). Se descarta el
            // valor de vuelta porque ya se conoce.
            declare_local(s.name, s.loc, s.decl_type);
            uint32_t var = static_cast<uint32_t>(s.slot);

            size_t start = chunk_->here();
            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(index));
            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(count));
            chunk_->emit(Op::Lt, s.loc);
            size_t to_end = chunk_->emit(Op::JumpIfFalse, s.loc);

            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(items));
            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(index));
            chunk_->emit(Op::GetIndex, s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, var);

            loops_.push_back({});
            emit_block(s.body);

            size_t step = chunk_->here();
            for (size_t j : loops_.back().continues) chunk_->patch(j, step);
            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(index));
            chunk_->emit(Op::Const, s.loc, chunk_->add_constant(Value::integer(1)));
            chunk_->emit(Op::Add, s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(index));
            chunk_->emit(Op::Jump, s.loc, static_cast<uint32_t>(start));

            chunk_->patch(to_end, chunk_->here());
            for (size_t j : loops_.back().breaks) chunk_->patch(j, chunk_->here());
            loops_.pop_back();

            end_scope();
            break;
        }

        case IrStmtKind::Try: {
            TryRange range;
            range.begin = chunk_->here();
            emit_block(s.body);
            range.end = chunk_->here();

            size_t to_end = chunk_->emit(Op::Jump, s.loc);
            range.catch_pc = chunk_->here();
            chunk_->try_ranges.push_back(range);

            begin_scope();
            if (!s.name.empty()) {
                // Mismo motivo que en For: declare_local hay que llamarlo
                // para que la ranura real coincida con la que ya calculo
                // check_stmt (s.slot); no se resuelve por nombre aqui.
                declare_local(s.name, s.loc);
                chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(s.slot));
            } else {
                chunk_->emit(Op::Pop, s.loc);
            }
            for (const auto& st : s.orelse) emit_stmt(*st);
            end_scope();

            chunk_->patch(to_end, chunk_->here());
            break;
        }
    }
}

} // namespace lumen_script
