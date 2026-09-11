#include <lumen_script/emitter.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <lumen_script/template.hpp>
#include <lumen_script/natives.hpp>

#include <algorithm>

namespace lumen_script {

void Emitter::error(SourceLoc loc, std::string msg) {
    diags_.error(loc, std::move(msg));
    failed_ = true;
}

int Emitter::declare_local(const std::string& name, SourceLoc loc,
                           const std::string& type) {
    for (auto it = locals_.rbegin(); it != locals_.rend(); ++it) {
        if (it->depth < scope_depth_) break;
        if (it->name == name) {
            error(loc, "'" + name + "' is already declared in this scope");
            return static_cast<int>(std::distance(locals_.begin(), it.base()) - 1);
        }
    }
    locals_.push_back({name, scope_depth_, type});
    int slot = static_cast<int>(locals_.size()) - 1;
    if (slot + 1 > chunk_->num_locals) chunk_->num_locals = slot + 1;
    chunk_->local_names.push_back(name);
    return slot;
}

const std::string& Emitter::local_type(const std::string& name) const {
    static const std::string kNone;
    for (int i = static_cast<int>(locals_.size()) - 1; i >= 0; --i)
        if (locals_[static_cast<size_t>(i)].name == name)
            return locals_[static_cast<size_t>(i)].type;
    return kNone;
}

bool Emitter::is_int_expr(const Expr& e) const {
    switch (e.kind) {
        case ExprKind::IntLit: return true;
        case ExprKind::Ident:  return local_type(e.text) == "int";
        case ExprKind::Binary:
            if (e.text == "+" || e.text == "-" || e.text == "*")
                return e.lhs && e.rhs && is_int_expr(*e.lhs) && is_int_expr(*e.rhs);
            return false;
        default: return false;
    }
}

// Collects the operands of a '+' chain in evaluation order.  It only walks down
// the left: the right-hand side enters as is, even if it is another '+' in
// parentheses, so as not to reassociate what the parser already associated.
void Emitter::flatten_concat(const Expr& e, std::vector<const Expr*>& out) {
    if (e.kind == ExprKind::Binary && e.text == "+" && e.lhs && e.rhs) {
        flatten_concat(*e.lhs, out);
        out.push_back(e.rhs.get());
        return;
    }
    out.push_back(&e);
}

// True if `e` IS, or is built by concatenating in, a direct call to
// query()/header() — the two builtins that hand back exactly what the
// client sent, unvalidated. No dataflow tracking: assigning the result to a
// local first (`string next = query("next")`) is not caught, on purpose —
// that shape at least gives the developer a place to put a check before the
// value reaches redirect(). This only catches the literal, unguarded splice,
// which is also the only shape with no legitimate reading.
bool Emitter::looks_like_direct_request_data(const Expr& e) {
    if (e.kind == ExprKind::Call && e.object &&
        e.object->kind == ExprKind::Ident &&
        (e.object->text == "query" || e.object->text == "header")) {
        return true;
    }
    if (e.kind == ExprKind::Binary && e.text == "+" && e.lhs && e.rhs) {
        return looks_like_direct_request_data(*e.lhs) ||
               looks_like_direct_request_data(*e.rhs);
    }
    return false;
}

int Emitter::resolve_local(const std::string& name) const {
    for (int i = static_cast<int>(locals_.size()) - 1; i >= 0; --i)
        if (locals_[static_cast<size_t>(i)].name == name) return i;
    return -1;
}

void Emitter::begin_scope() { ++scope_depth_; }

void Emitter::end_scope() {
    --scope_depth_;
    // Slots are not recycled: the cost is one more entry in the locals vector
    // and in exchange the indices are stable, which simplifies the VM.
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

    for (const auto& p : route.params) declare_local(p.name, p.loc, p.type.name);

    // The group guards are emitted before the body, outside in: to reach the
    // handler you must first pass the parent group's.  Each one is the same
    // `if not X: return Y` as `require`, so there is no middleware concept in
    // the emitter or in the VM.
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

    // A handler that runs off the end returns nothing: the engine will reply
    // with whatever a builtin wrote, or 204 if it wrote nothing.
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

    for (const auto& p : fn.params) declare_local(p.name, p.loc, p.type.name);

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

    // `this` is simply parameter 0.
    declare_local("this", m.loc, cls);
    for (const auto& p : m.params) declare_local(p.name, p.loc, p.type.name);

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

    for (const auto& p : ct.params) declare_local(p.name, p.loc, p.type.name);
    int self = declare_local("this", ct.loc, cls);

    // The instance starts with every declared field set to null, so reading one
    // the constructor does not touch gives null instead of failing.
    for (const auto& f : fields) {
        chunk_->emit(Op::Const, ct.loc, chunk_->add_constant(Value::str(f)));
        chunk_->emit(Op::Const, ct.loc, chunk_->add_constant(Value::null()));
    }
    chunk_->emit(Op::MakeDict, ct.loc, static_cast<uint32_t>(fields.size()));
    chunk_->emit(Op::StoreLocal, ct.loc, static_cast<uint32_t>(self));

    if (ct.has_body) {
        emit_block(ct.body);
    } else {
        // No body: each parameter goes to the field of the same name.
        for (const auto& p : ct.params) {
            if (std::find(fields.begin(), fields.end(), p.name) == fields.end()) {
                error(p.loc, "'" + p.name + "' is not a field of '" + cls + "'");
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

bool Emitter::emit_condition(const Expr& e, const std::vector<TypedName>& names,
                             Chunk& out) {
    chunk_        = &out;
    route_method_ = {};
    failed_       = false;
    locals_.clear();
    loops_.clear();
    scope_depth_ = 0;

    for (const auto& n : names) declare_local(n.name, e.loc, n.type);

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
            int slot = declare_local(s.name, s.loc, s.type.name);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(slot));
            break;
        }

        case StmtKind::Assign: {
            // `session.x = v` → __session_set("x", v).  It is the only assignment
            // to a member that exists; the rest of the reserved objects are
            // read-only.
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

            // `xs[0] = v` and `d["k"] = v`.
            if (s.target->kind == ExprKind::Index) {
                emit_expr(*s.target->object);
                emit_expr(*s.target->lhs);
                emit_expr(*s.value);
                chunk_->emit(Op::SetIndex, s.loc);
                chunk_->emit(Op::Pop, s.loc);
                return;
            }

            // `this.field = v` and `object.field = v`.
            if (s.target->kind == ExprKind::Member) {
                if (!check_field(*s.target->object, s.target->text, s.loc)) return;
                emit_expr(*s.target->object);
                emit_expr(*s.value);
                chunk_->emit(Op::SetMember, s.loc,
                             chunk_->add_constant(Value::str(s.target->text)));
                chunk_->emit(Op::Pop, s.loc);
                return;
            }

            if (s.target->kind != ExprKind::Ident) {
                error(s.loc, "can only assign to a variable or a field");
                return;
            }
            int slot = resolve_local(s.target->text);
            if (slot < 0) {
                error(s.target->loc, "'" + s.target->text + "' is not declared");
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
            // In a while, `continue` re-evaluates the condition.
            for (size_t j : loops_.back().continues) chunk_->patch(j, start);
            chunk_->emit(Op::Jump, s.loc, static_cast<uint32_t>(start));
            chunk_->patch(to_end, chunk_->here());

            for (size_t j : loops_.back().breaks) chunk_->patch(j, chunk_->here());
            loops_.pop_back();
            break;
        }

        // `require X else Y` is sugar for `if not X: return Y`.  It is emitted
        // as such: no dedicated opcode and no middleware concept in the VM.
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
            if (loops_.empty()) { error(s.loc, "'break' outside a loop"); return; }
            loops_.back().breaks.push_back(chunk_->emit(Op::Jump, s.loc));
            break;

        case StmtKind::Continue:
            if (loops_.empty()) { error(s.loc, "'continue' outside a loop"); return; }
            loops_.back().continues.push_back(chunk_->emit(Op::Jump, s.loc));
            break;

        // `for T x in xs:` is desugared into an index over the list.  The helper
        // slots carry a space in the name, which no Lumen Script identifier can
        // contain: that way they never clash with the user's or between two
        // nested loops.
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

            int var = declare_local(s.name, s.loc, s.type.name);

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

            // `continue` jumps to the increment, not to the start: otherwise the
            // loop would never advance.
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

            // The error arrives on top as a Dict with `message`.
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
                    error(e.loc, "'" + e.text + "' is a builtin: it must be called, "
                                 "not to use it as a value");
                else
                    error(e.loc, "'" + e.text + "' is not declared");
                return;
            }
            chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(slot));
            break;
        }

        case ExprKind::Unary:
            emit_expr(*e.lhs);
            chunk_->emit(e.text == "not" ? Op::Not : Op::Neg, e.loc);
            break;

        // and/or short-circuit: the top is peeked without consuming it and the
        // jump happens with it in place, which is exactly the result value.
        case ExprKind::Binary: {
            if (e.text == "and" || e.text == "or") {
                emit_expr(*e.lhs);
                size_t j = chunk_->emit(e.text == "and" ? Op::JumpIfFalsePeek
                                                        : Op::JumpIfTruePeek, e.loc);
                emit_expr(*e.rhs);
                chunk_->patch(j, chunk_->here());
                break;
            }

            // A chain of '+' collapses into a single ConcatN: that way the text
            // is built in a buffer with the size already reserved, instead of
            // creating a box and a buffer per '+' and throwing all but the last.
            //
            // Only the left spine, which is how the parser associates: flattening
            // the right as well would change the order of a float sum, where
            // (a+b)+c is not a+(b+c).  Integer chains keep their AddInt, which is
            // already the fast path for that case.
            if (e.text == "+" && !is_int_expr(e)) {
                std::vector<const Expr*> parts;
                flatten_concat(e, parts);
                if (parts.size() >= 3) {
                    for (const Expr* p : parts) emit_expr(*p);
                    chunk_->emit(Op::ConcatN, e.loc,
                                 static_cast<uint32_t>(parts.size()));
                    break;
                }
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
            else { error(e.loc, "unsupported operator: " + e.text); return; }

            // Lumen Script declares the types, so what the generic VM works out
            // on every pass is known here once.
            if (is_int_expr(*e.lhs) && is_int_expr(*e.rhs)) {
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

        // `sse.open` is not a dictionary field: it is a reserved object, and it
        // resolves to the builtin implementing it.
        case ExprKind::Member: {
            // `session.x` accepts any name: it is a store, not an object with
            // fixed members.  It translates to __session_get("x").
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
                    error(e.loc, "'" + e.object->text + "' has no member '" +
                                 e.text + "'");
                    return;
                }
                if (native_at(id).min_args > 0) {
                    error(e.loc, "'" + e.object->text + "." + e.text +
                                 "' is an operation: it must be called with ()");
                    return;
                }
                if ((e.object->text == "sse" && route_method_ != "SSE") ||
                    (e.object->text == "ws"  && route_method_ != "WS")) {
                    error(e.loc, "'" + e.object->text + "' only exists inside "
                                 "a route " + e.object->text);
                    return;
                }
                if (e.object->text == "error" && route_method_ != "ERROR") {
                    error(e.loc, "'error' only exists inside an 'on error'");
                    return;
                }
                chunk_->emit(Op::CallNative, e.loc,
                             static_cast<uint32_t>(id) << 8);
                return;
            }
            if (!check_field(*e.object, e.text, e.loc)) return;

            emit_expr(*e.object);
            chunk_->emit(Op::GetMember, e.loc,
                         chunk_->add_constant(Value::str(e.text)));
            break;
        }

        case ExprKind::Call:
            emit_call(e, /*awaited=*/false);
            break;

        // `await` only makes sense on a call to a builtin that suspends.
        // Anything else is rejected here, not at runtime.
        // ++x / x++ on a variable: it is read twice, which is cheaper than
        // duplicating on the stack and needs no new opcode.
        //
        // On a field, the receiver is kept in a helper slot: `o.f++` cannot
        // evaluate `o` twice, because `o` may have side effects.
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
                if (slot < 0) { error(tgt.loc, "'" + tgt.text + "' is not declared"); return; }
                uint32_t u = static_cast<uint32_t>(slot);

                if (post) chunk_->emit(Op::LoadLocal, e.loc, u);   // value anterior
                chunk_->emit(Op::LoadLocal, e.loc, u);
                one();
                chunk_->emit(op, e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, u);
                if (!post) chunk_->emit(Op::LoadLocal, e.loc, u);  // value nuevo
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

                // SetMember leaves the new value on top.
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

                // SetIndex leaves the new value on top.
                if (post) {
                    chunk_->emit(Op::Pop, e.loc);
                    chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(prev));
                }
                end_scope();
                break;
            }

            error(e.loc, "'++' and '--' only apply to a variable, a field "
                         "or an indexed element");
            break;
        }

        case ExprKind::Await: {
            if (!e.lhs || e.lhs->kind != ExprKind::Call) {
                error(e.loc, "'await' only applies to an asynchronous call "
                             "(for now: sleep)");
                return;
            }
            emit_call(*e.lhs, /*awaited=*/true);
            break;
        }

        case ExprKind::This: {
            int slot = resolve_local("this");
            if (slot < 0) {
                error(e.loc, "'this' only exists inside a method or a constructor");
                return;
            }
            chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(slot));
            break;
        }
    }
}

// A field on a receiver of known type.  Good for both reading and writing it:
// a class's fields are the declared ones, and if the write were not checked you
// could create one that then cannot be read.
//
// Without the check, reading a field that does not exist silently returns null
// and the failure shows up pages later, far from the typo.
bool Emitter::check_field(const Expr& object, const std::string& field,
                              SourceLoc loc) {
    const std::string tr = type_of(object);
    if (tr.empty()) return true;

    if (classes_) {
        auto it = classes_->find(tr);
        if (it != classes_->end()) {
            const auto& f = it->second.fields;
            if (std::find(f.begin(), f.end(), field) != f.end()) return true;
            if (it->second.methods.count(field)) {
                error(loc, "'" + tr + "." + field + "' es un method: "
                           "it must be called with ()");
            } else {
                std::string available;
                for (const auto& n : f) available += (available.empty() ? "" : ", ") + n;
                error(loc, "'" + tr + "' has no field '" + field + "'" +
                           (available.empty() ? "" : "; it has " + available));
            }
            return false;
        }
    }
    // A scalar or a List has no fields, whatever they are asked for.  A Dict
    // does: there any key is valid.
    if (methods_of(tr) && tr != "Dict") {
        error(loc, "'" + field + "' on " + tr + ", which has no fields");
        return false;
    }
    return true;
}

// Only the obvious: a literal, or a variable with a declared type.  There is no
// inference, so when in doubt it returns "" and nothing is checked.
std::string Emitter::type_of(const Expr& e) const {
    switch (e.kind) {
        case ExprKind::StringLit: return "string";
        case ExprKind::IntLit:    return "int";
        case ExprKind::FloatLit:  return "float";
        case ExprKind::BoolLit:   return "bool";
        case ExprKind::Ident:     return local_type(e.text);
        case ExprKind::This:      return local_type("this");
        // Builtin method on a receiver of known type: the chain continues.
        case ExprKind::Call: {
            if (!e.object || e.object->kind != ExprKind::Member) return {};
            const std::string recv = type_of(*e.object->object);
            const auto* list_ = methods_of(recv);
            if (!list_) return {};
            for (const auto& m : *list_)
                if (e.object->text == m.name)
                    return m.devuelve ? m.devuelve : recv;
            return {};
        }
        default:                  return {};
    }
}

bool Emitter::check_builtin_method(const Expr& e) {
    const auto* list_ = methods_of(type_of(*e.object->object));
    if (!list_) return true;                  // type with no closed list

    const std::string& method = e.object->text;
    const BuiltinMethod* def = nullptr;
    for (const auto& m : *list_)
        if (method == m.name) { def = &m; break; }

    if (!def) {
        std::string available;
        for (const auto& m : *list_) available += (available.empty() ? "" : ", ") + std::string(m.name);
        error(e.object->loc, "values of type " + type_of(*e.object->object) +
                             " have no method '" + method + "'; it has " + available);
        return false;
    }

    size_t argc = 0, named = 0;
    for (const auto& a : e.args) (a.name.empty() ? argc : named)++;
    if (named > 0) ++argc;                    // van agrupados en un Dict

    if (static_cast<int>(argc) < def->min_args ||
        static_cast<int>(argc) > def->max_args) {
        std::string expects = std::to_string(def->min_args);
        if (def->max_args != def->min_args) expects += "-" + std::to_string(def->max_args);
        error(e.loc, "'" + method + "()' expects " + expects +
                     " argument(s), but receives " + std::to_string(argc));
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
    // Named arguments are grouped into a Dict that takes the last positional
    // slot, the same as in render().
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
    if (!e.object) { error(e.loc, "call without a target"); return; }

    std::string name;
    int         id = -1;

    // sse.send(...) — member of a reserved object.
    if (e.object->kind == ExprKind::Member &&
        e.object->object->kind == ExprKind::Ident &&
        resolve_local(e.object->object->text) < 0 &&
        is_reserved_object(e.object->object->text)) {

        name = e.object->object->text + "." + e.object->text;
        id   = member_native_id(e.object->object->text, e.object->text);
        if (id < 0) {
            error(e.object->loc, "'" + e.object->object->text +
                                 "' has no member '" + e.object->text + "'");
            return;
        }
        const std::string& obj = e.object->object->text;

        // Modules: `sqlite.query(...)` requires `import sqlite`, and the module
        // name travels as the first argument so that a single builtin serves
        // all three.
        bool is_module = is_db_module(obj);
        if (is_module && (!imports_ || !imports_->count(obj))) {
            error(e.object->loc, "missing 'import " + obj + "' in order to use '" +
                                 obj + "." + e.object->text + "'");
            return;
        }
        if (is_module) {
            const NativeDef& mdef = native_at(id);
            if (!awaited) {
                error(e.loc, "'" + obj + "." + e.object->text + "()' is asynchronous: "
                             "you must write 'await " + obj + "." + e.object->text +
                             "(...)'");
                return;
            }

            // Gluing a value into the SQL string is the one way left to
            // reintroduce injection in an API that is parameterised by
            // default: query()/exec() take the values as extra arguments and
            // bind them through the driver (sqlite3_bind_*, PQexecParams,
            // mysql_stmt_bind_param), and a '+' with a non-literal operand
            // walks straight past all of it.
            if ((e.object->text == "query" || e.object->text == "exec") &&
                !e.args.empty() && e.args[0].value &&
                e.args[0].value->kind == ExprKind::Binary &&
                e.args[0].value->text == "+") {

                // Unambiguous case: query()/header() spliced straight into
                // the SQL text with nothing in between. There is no reading
                // of this that isn't the injection — hard error, same as
                // redirect() above.
                if (looks_like_direct_request_data(*e.args[0].value)) {
                    error(e.args[0].value->loc,
                          "request data glued directly into SQL text in '" + obj + "." +
                          e.object->text + "()' — this is SQL injection. Pass the "
                          "value as an extra argument and write '?' in the query "
                          "instead: it goes through the driver's bind, not the "
                          "string.");
                    return;
                }

                // Everything else that concatenates in a non-literal is only
                // a warning: '... in (' + marks + ')' with a placeholder
                // list built at runtime is legitimate and has no other
                // spelling, so refusing to compile it would be wrong.
                // Concatenating only string literals is left alone too —
                // that is just a long query split over several lines.
                std::vector<const Expr*> partes;
                flatten_concat(*e.args[0].value, partes);
                bool interpolado = false;
                for (const Expr* p : partes)
                    if (p->kind != ExprKind::StringLit) { interpolado = true; break; }
                if (interpolado) {
                    const SourceLoc& l = e.args[0].value->loc;
                    std::cerr << "lumen: warning: " << (l.file ? *l.file : "?")
                              << ":" << l.line << ":" << l.col
                              << ": SQL built by concatenation in '" << obj << '.'
                              << e.object->text << "()'; pass the value as an argument "
                                 "and write '?' in the query, or it goes in unescaped\n";
                }
            }
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::str(obj)));
            size_t argc = 1;
            for (const auto& a : e.args) {
                if (!a.name.empty()) {
                    error(a.loc, "queries do not accept named arguments");
                    return;
                }
                emit_expr(*a.value);
                ++argc;
            }
            if (argc < static_cast<size_t>(mdef.min_args)) {
                error(e.loc, "'" + obj + "." + e.object->text +
                             "()' expects at least the SQL query");
                return;
            }
            if (mdef.max_args >= 0 && argc > static_cast<size_t>(mdef.max_args)) {
                error(e.loc, "'" + obj + "." + e.object->text +
                             "()' takes no arguments");
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
            error(e.object->loc, "'" + obj + "' only exists inside a route " + obj);
            return;
        }
        if (obj == "error" && route_method_ != "ERROR") {
            error(e.object->loc, "'error' only exists inside an 'on error'");
            return;
        }
    }
    else if (e.object->kind == ExprKind::Ident) {
        name = e.object->text;
        id   = native_id(name);

        // If it is not a builtin, it may be a user function.  Declaring one with
        // a builtin's name is already rejected when the table is built, so there
        // is no ambiguity to resolve here.
        if (id < 0) {
            auto it = functions_ ? functions_->find(name) : FunctionSigs::const_iterator();
            if (functions_ && it != functions_->end()) {
                const FnSig& sig = it->second;
                size_t given = 0;
                for (const auto& a : e.args) {
                    if (!a.name.empty()) {
                        error(a.loc, "a user function does not accept arguments "
                                     "with a name");
                        return;
                    }
                    emit_expr(*a.value);
                    ++given;
                }

                if (given < sig.required || given > sig.defaults.size()) {
                    std::string expected = std::to_string(sig.required);
                    if (sig.defaults.size() != sig.required)
                        expected += " a " + std::to_string(sig.defaults.size());
                    error(e.loc, "'" + name + "()' expects " + expected +
                                 " argument(s), but receives " + std::to_string(given));
                    return;
                }

                // The missing ones are filled in here with their default value:
                // the function always receives the complete list.
                for (size_t i = given; i < sig.defaults.size(); ++i)
                    emit_expr(*sig.defaults[i]);

                size_t argc = sig.defaults.size();
                if (argc > 255) { error(e.loc, "demasiados argumentos"); return; }
                chunk_->emit(Op::CallFunction, e.loc,
                             (static_cast<uint32_t>(sig.index) << 8) |
                             static_cast<uint32_t>(argc));
                return;
            }
            // Constructor: a call to a class name.
            auto ct = classes_ ? classes_->find(name) : ClassSigs::const_iterator();
            if (classes_ && ct != classes_->end()) {
                size_t argc = 0;
                for (const auto& a : e.args) {
                    if (!a.name.empty()) {
                        error(a.loc, "a constructor does not accept named arguments");
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
                    error(e.loc, "'" + name + "' has no constructor taking " +
                                 std::to_string(argc) + " parametro(s)" +
                                 (opciones.empty() ? "" : "; there are ones taking " + opciones));
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
    // Class method: the receiver's declared type is known at compile time, so
    // it is resolved here and a misspelled name never reaches production.
    else if (e.object->kind == ExprKind::Member && classes_) {
        std::string recv_type;
        if (e.object->object->kind == ExprKind::Ident)
            recv_type = local_type(e.object->object->text);
        else if (e.object->object->kind == ExprKind::This)
            recv_type = local_type("this");

        auto cls = recv_type.empty() ? classes_->end() : classes_->find(recv_type);
        if (cls != classes_->end()) {
            auto m = cls->second.methods.find(e.object->text);
            if (m == cls->second.methods.end()) {
                // It may be a field with a generic method on top, or an error.
                if (!cls->second.fields.empty() &&
                    std::find(cls->second.fields.begin(), cls->second.fields.end(),
                              e.object->text) == cls->second.fields.end()) {
                    error(e.object->loc, "'" + recv_type + "' has no method '" +
                                         e.object->text + "'");
                    return;
                }
            } else {
                const FnSig& sig = m->second;
                emit_expr(*e.object->object);          // `this`
                size_t given = 0;
                for (const auto& a : e.args) {
                    if (!a.name.empty()) {
                        error(a.loc, "a method does not accept named arguments");
                        return;
                    }
                    emit_expr(*a.value);
                    ++given;
                }
                if (given < sig.required || given > sig.defaults.size()) {
                    error(e.loc, "'" + recv_type + "." + e.object->text +
                                 "()' expects " + std::to_string(sig.required) +
                                 " argument(s), but receives " + std::to_string(given));
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
        if (!check_builtin_method(e)) return;
        emit_method_call_dynamic(e);
        return;
    }
    else if (e.object->kind == ExprKind::Member) {
        if (!check_builtin_method(e)) return;
        emit_method_call_dynamic(e);
        return;
    }
    else {
        error(e.loc, "for now only builtins or methods can be called");
        return;
    }

    const NativeDef& def = native_at(id);

    // redirect(query("next")) / redirect(header("referer") + "/x") is the
    // textbook open redirect: whatever the client sends becomes the
    // Location header verbatim. There is no legitimate reason to splice
    // query()/header() straight into a redirect target — a route that
    // genuinely needs to bounce somewhere request-dependent validates it
    // first (an allowlist, an if/else over a fixed set of literals) and
    // hands redirect() the already-checked local variable, which this does
    // not flag. redirect(path, code) — a hardcoded/config-derived target,
    // by far the common case — is untouched.
    if (name == "redirect" && !e.args.empty() && e.args[0].value &&
        looks_like_direct_request_data(*e.args[0].value)) {
        error(e.loc, "redirect() target comes straight from query()/header(): "
                     "an attacker controls it and can point it anywhere "
                     "('open redirect'). Validate it first (compare against an "
                     "allowlist or a fixed set of literals) and pass that "
                     "checked value instead.");
        return;
    }

    // A builtin that suspends forces you to await it, and one that does not
    // will not accept await: that way the call signature always says whether
    // the handler can stop there, without consulting the builtin table.
    if (def.is_async && !awaited) {
        error(e.loc, "'" + name + "()' is asynchronous: available kind write "
                     "'await " + name + "(...)'");
        return;
    }
    if (!def.is_async && awaited) {
        error(e.loc, "'" + name + "()' is not asynchronous: the 'await' is unnecessary");
        return;
    }

    size_t positional = 0, named = 0;
    for (const auto& a : e.args) (a.name.empty() ? positional : named)++;

    // Named arguments are the template variables: they are grouped into a Dict
    // that takes the last positional slot.
    if (named > 0 && name != "render") {
        error(e.loc, "'" + name + "()' no admite argumentos with a name");
        return;
    }

    // ── render() with a literal name: the template is compiled HERE ─────────
    //
    // The emitter has the file name and the keys passed to it right there,
    // which is exactly what it needs.  Compiling now turns a typo inside a
    // {{ }} into a `lumen --check` error.
    //
    // If the name is not literal —render(variable)— there is nothing to compile
    // ahead of time and it goes down the old path.
    if (name == "render") {
        if (e.args.empty() || !e.args[0].name.empty() ||
            e.args[0].value->kind != ExprKind::StringLit) {
            // The name has to be written out.  Otherwise there is nothing to
            // compile ahead of time, and a template only checked when someone
            // loads the page is worthless: half the point of having them in
            // Lumen Script is that their typos are compile errors.
            //
            error(e.loc, "render() needs the template name written out, not a "
                         "variable. To choose between several, use an if with literal "
                         "literals: every branch is checked at compile time");
            return;
        }
        if (!templates_ || !templates_->table) {
            error(e.loc, "render() cannot be used here");
            return;
        }
        emit_compiled_render(e);
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
            expected += def.max_args < 0 ? " or more"
                                         : "-" + std::to_string(def.max_args);
        error(e.loc, "'" + name + "()' expects " + expected +
                     " argument(s), but receives " + std::to_string(argc));
        return;
    }
    if (argc > 255) { error(e.loc, "demasiados argumentos"); return; }

    if (def.is_async) chunk_->has_await = true;

    chunk_->emit(def.is_async ? Op::CallAsync : Op::CallNative, e.loc,
                 (static_cast<uint32_t>(id) << 8) | static_cast<uint32_t>(argc));
}

// Compiles the template against this call's keys and emits a call to
// __render_tpl(indice, data).
void Emitter::emit_compiled_render(const Expr& e) {
    const std::string& name = e.args[0].value->text;

    if (name.find("..") != std::string::npos ||
        std::filesystem::path(name).is_absolute()) {
        error(e.args[0].loc, "invalid template name: '" + name + "'");
        return;
    }

    // Every key travels with the type of what is passed to it: that is what
    // makes `{{ title.mayusculas() }}` inside the template an error here and
    // not a broken page in production.
    std::vector<TypedName> keys;
    for (const auto& a : e.args) {
        if (a.name.empty()) continue;
        keys.push_back({a.name, type_of(*a.value)});
    }

    const std::filesystem::path path =
        std::filesystem::path(templates_->dir) / name;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error(e.args[0].loc, "template not found: '" + name + "' en " +
                             templates_->dir);
        return;
    }
    const std::string fuente((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

    Template tpl;
    if (!compilar_plantilla(fuente, name, templates_->dir, keys, diags_, tpl)) {
        failed_ = true;
        return;
    }
    const uint32_t idx = static_cast<uint32_t>(templates_->table->size());
    templates_->table->push_back(std::move(tpl));

    // __render_tpl(indice, data)
    chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::integer(idx)));
    for (const auto& a : e.args) {
        if (a.name.empty()) continue;
        chunk_->emit(Op::Const, a.loc, chunk_->add_constant(Value::str(a.name)));
        emit_expr(*a.value);
    }
    chunk_->emit(Op::MakeDict, e.loc, static_cast<uint32_t>(keys.size()));

    const int id = native_id("__render_tpl");
    chunk_->emit(Op::CallNative, e.loc, (static_cast<uint32_t>(id) << 8) | 2u);
}

} // namespace lumen_script
