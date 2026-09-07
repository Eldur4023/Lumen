#include "http_parser.hpp"
#include <llhttp.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace lumen::http {

// ── Per-connection parse state ─────────────────────────────────────────────

struct HttpParser::ParseContext {
    ParsedRequest current;

    // Header accumulation
    std::string last_field;
    std::string last_value;
    bool        value_pending = false;  // last_value not yet committed to map
    size_t      header_count  = 0;

    // Flag set by any callback that detects a limit violation
    bool error = false;

    // Set specifically when the body exceeds kMaxBodySize, so the connection
    // layer can answer 413 instead of a generic 400 — the client sent a
    // well-formed request that is simply too big, not a malformed one.
    bool body_too_large = false;

    // Back-pointer to the owning parser's OnComplete (stable address)
    OnComplete* on_complete = nullptr;
};

// ── llhttp callbacks ───────────────────────────────────────────────────────

static HttpParser::ParseContext* ctx(llhttp_t* p) {
    return static_cast<HttpParser::ParseContext*>(p->data);
}

static int cb_on_url(llhttp_t* p, const char* at, size_t len) {
    auto* c = ctx(p);
    if (c->current.path.size() + len > kMaxUrlSize) { c->error = true; return HPE_USER; }
    c->current.path.append(at, len);
    return HPE_OK;
}

static void commit_header(HttpParser::ParseContext* c) {
    if (!c->value_pending) return;
    std::string key = c->last_field;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });

    auto it = c->current.headers.find(key);
    if (it != c->current.headers.end()) {
        // RFC 7230 §3.2.2: duplicate headers may be combined with ", ".
        // set-cookie is the sole exception — each value must stay on its own line.
        // In practice set-cookie appears in responses, not requests, but guard anyway.
        const char* sep = (key == "set-cookie") ? "\n" : ", ";
        it->second += sep;
        it->second += c->last_value;
    } else {
        c->current.headers[key] = std::move(c->last_value);
    }

    c->last_field.clear();
    c->last_value.clear();
    c->value_pending = false;
    ++c->header_count;
}

static int cb_on_header_field(llhttp_t* p, const char* at, size_t len) {
    auto* c = ctx(p);
    // A new field name means the previous field+value pair is complete
    if (c->value_pending) {
        if (c->header_count >= kMaxHeaderCount) { c->error = true; return HPE_USER; }
        commit_header(c);
    }
    if (c->last_field.size() + len > kMaxHeaderSize) { c->error = true; return HPE_USER; }
    c->last_field.append(at, len);
    return HPE_OK;
}

static int cb_on_header_value(llhttp_t* p, const char* at, size_t len) {
    auto* c = ctx(p);
    if (c->last_value.size() + len > kMaxHeaderSize) { c->error = true; return HPE_USER; }
    c->last_value.append(at, len);
    c->value_pending = true;
    return HPE_OK;
}

static int cb_on_headers_complete(llhttp_t* p) {
    auto* c = ctx(p);
    // Commit the final header
    if (c->value_pending) {
        if (c->header_count >= kMaxHeaderCount) { c->error = true; return HPE_USER; }
        commit_header(c);
    }
    // HTTP version
    int major = llhttp_get_http_major(p);
    int minor = llhttp_get_http_minor(p);
    c->current.version = (major == 1 && minor == 0) ? "HTTP/1.0" : "HTTP/1.1";
    return HPE_OK;
}

static int cb_on_body(llhttp_t* p, const char* at, size_t len) {
    auto* c = ctx(p);
    if (c->current.body.size() + len > kMaxBodySize) {
        c->error = true;
        c->body_too_large = true;
        return HPE_USER;
    }
    c->current.body.append(at, len);
    return HPE_OK;
}

static int cb_on_message_complete(llhttp_t* p) {
    auto* c = ctx(p);

    // Split path from query string
    auto q = c->current.path.find('?');
    if (q != std::string::npos) {
        c->current.query = c->current.path.substr(q + 1);
        c->current.path  = c->current.path.substr(0, q);
    }

    // Method name
    c->current.method = llhttp_method_name(
        static_cast<llhttp_method_t>(llhttp_get_method(p)));

    (*c->on_complete)(std::move(c->current));

    // Reset per-message state (keep parser alive for keep-alive)
    c->current      = {};
    c->header_count = 0;

    // Pause so the connection layer can serialise pipelined requests: the
    // bytes that follow this message stay in the caller's buffer until the
    // current response is on the wire.
    return HPE_PAUSED;
}

// ── HttpParser ─────────────────────────────────────────────────────────────

HttpParser::HttpParser(OnComplete on_complete)
    : on_complete_(std::move(on_complete))
    , ctx_(std::make_unique<ParseContext>())
    , parser_(std::make_unique<llhttp_t>())
    , settings_(std::make_unique<llhttp_settings_t>())
{
    ctx_->on_complete = &on_complete_;

    llhttp_settings_init(settings_.get());
    settings_->on_url              = cb_on_url;
    settings_->on_header_field     = cb_on_header_field;
    settings_->on_header_value     = cb_on_header_value;
    settings_->on_headers_complete = cb_on_headers_complete;
    settings_->on_body             = cb_on_body;
    settings_->on_message_complete = cb_on_message_complete;

    llhttp_init(parser_.get(), HTTP_REQUEST, settings_.get());
    parser_->data = ctx_.get();
}

HttpParser::~HttpParser() = default;

bool HttpParser::feed(const char* data, size_t len) {
    if (ctx_->error) return false;
    last_data_ = data;
    last_len_  = len;
    llhttp_errno_t err = llhttp_execute(parser_.get(), data, len);
    if (err == HPE_OK)     return !ctx_->error;
    if (err == HPE_PAUSED) return true;   // request completed → paused for serialisation
    return false;
}

bool HttpParser::is_paused() const {
    return llhttp_get_errno(parser_.get()) == HPE_PAUSED;
}

bool HttpParser::body_too_large() const {
    return ctx_->body_too_large;
}

size_t HttpParser::unconsumed() const {
    if (!is_paused() || !last_data_) return 0;
    const char* pos = llhttp_get_error_pos(parser_.get());
    if (pos == nullptr || pos < last_data_ || pos > last_data_ + last_len_) return 0;
    return last_len_ - static_cast<size_t>(pos - last_data_);
}

void HttpParser::resume() {
    if (is_paused()) llhttp_resume(parser_.get());
    last_data_ = nullptr;
    last_len_  = 0;
}

void HttpParser::reset() {
    ctx_->current      = {};
    ctx_->last_field.clear();
    ctx_->last_value.clear();
    ctx_->value_pending = false;
    ctx_->header_count  = 0;
    ctx_->error         = false;
    ctx_->body_too_large = false;
    last_data_ = nullptr;
    last_len_  = 0;
    llhttp_reset(parser_.get());
}

} // namespace lumen::http
