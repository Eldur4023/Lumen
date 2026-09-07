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

bool Emitter::emit_route(const RouteDecl& route, Chunk& out) {
    chunk_        = &out;
    route_method_ = route.method;
    failed_       = false;
    locals_.clear();
    loops_.clear();
    scope_depth_ = 0;

    for (const auto& p : route.params) declare_local(p.name, p.loc, Type::from_declared(p.type));

    // Las guardas del grupo se emiten antes del cuerpo, de fuera hacia dentro:
    // para llegar al handler hay que pasar primero la del grupo padre.  Cada
    // una es el mismo `if not X: return Y` que `require`, asi que no hay
    // concepto de middleware ni en el emisor ni en el VM.
    for (const auto& g : route.guards) {
        if (!g.condition || !g.otherwise) continue;
        emit_expr(*g.condition);
        size_t to_ok = chunk_->emit(Op::JumpIfFalse, g.loc);
        size_t skip  = chunk_->emit(Op::Jump, g.loc);
        chunk_->patch(to_ok, chunk_->here());
        emit_expr(*g.otherwise);
        chunk_->emit(Op::Return, g.loc);
        chunk_->patch(skip, chunk_->here());
    }

    emit_block(route.body);

    // Un handler que se cae por el final no devuelve nada: el motor respondera
    // con lo que haya escrito un builtin, o 204 si no escribio nada.
    out.emit(Op::ReturnNull, route.loc);
    return !failed_;
}

bool Emitter::emit_function(const FnDecl& fn, Chunk& out) {
    chunk_        = &out;
    route_method_ = "FN";
    failed_       = false;
    locals_.clear();
    loops_.clear();
    scope_depth_ = 0;

    for (const auto& p : fn.params) declare_local(p.name, p.loc, Type::from_declared(p.type));

    emit_block(fn.body);
    out.emit(Op::ReturnNull, fn.loc);
    return !failed_;
}

bool Emitter::emit_method(const std::string& cls, const FnDecl& m, Chunk& out) {
    chunk_        = &out;
    route_method_ = "FN";
    failed_       = false;
    locals_.clear();
    loops_.clear();
    scope_depth_ = 0;

    // `this` es simplemente el parametro 0.
    declare_local("this", m.loc, Type::class_ref(cls));
    for (const auto& p : m.params) declare_local(p.name, p.loc, Type::from_declared(p.type));

    emit_block(m.body);
    out.emit(Op::ReturnNull, m.loc);
    return !failed_;
}

bool Emitter::emit_ctor(const std::string& cls, const std::vector<std::string>& fields,
                        const CtorDecl& ct, Chunk& out) {
    chunk_        = &out;
    route_method_ = "FN";
    failed_       = false;
    locals_.clear();
    loops_.clear();
    scope_depth_ = 0;

    for (const auto& p : ct.params) declare_local(p.name, p.loc, Type::from_declared(p.type));
    int self = declare_local("this", ct.loc, Type::class_ref(cls));

    // La instancia arranca con todos los campos declarados a null, para que
    // acceder a uno que el constructor no toque de null y no falle.
    for (const auto& f : fields) {
        chunk_->emit(Op::Const, ct.loc, chunk_->add_constant(Value::str(f)));
        chunk_->emit(Op::Const, ct.loc, chunk_->add_constant(Value::null()));
    }
    chunk_->emit(Op::MakeDict, ct.loc, static_cast<uint32_t>(fields.size()));
    chunk_->emit(Op::StoreLocal, ct.loc, static_cast<uint32_t>(self));

    if (ct.has_body) {
        emit_block(ct.body);
    } else {
        // Sin cuerpo: cada parametro va al campo de su mismo nombre.
        for (const auto& p : ct.params) {
            if (std::find(fields.begin(), fields.end(), p.name) == fields.end()) {
                error(p.loc, "'" + p.name + "' no es un campo de '" + cls + "'");
                continue;
            }
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
    chunk_        = &out;
    route_method_ = {};
    failed_       = false;
    locals_.clear();
    loops_.clear();
    scope_depth_ = 0;

    for (const auto& n : names) declare_local(n.nombre, e.loc, Type::from_legacy_name(n.tipo));

    emit_expr(e);
    out.emit(Op::Return, e.loc);
    return !failed_;
}

bool Emitter::emit_error_handler(const ErrorDecl& decl, Chunk& out) {
    chunk_        = &out;
    route_method_ = "ERROR";
    failed_       = false;
    locals_.clear();
    loops_.clear();
    scope_depth_ = 0;

    emit_block(decl.body);
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

} // namespace lumen_script
