#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>
#include <cstddef>

// Forward-declare llhttp types to avoid pulling the header into every TU
// that includes this file.
struct llhttp__internal_s;
typedef struct llhttp__internal_s llhttp_t;
struct llhttp_settings_s;
typedef struct llhttp_settings_s llhttp_settings_t;

namespace lumen::http {

// Parsed representation of one complete HTTP/1.1 request.
struct ParsedRequest {
    std::string method;
    std::string path;
    std::string query;    // raw query string
    std::string version;  // "HTTP/1.1" or "HTTP/1.0"
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

// ── Security limits ────────────────────────────────────────────────────────
inline constexpr size_t kMaxUrlSize     =  8 * 1024;   //  8 KB
inline constexpr size_t kMaxHeaderSize  =  8 * 1024;   //  8 KB per field/value
inline constexpr size_t kMaxHeaderCount =  100;
inline constexpr size_t kMaxBodySize    = 16 * 1024 * 1024; // 16 MB

// Incremental HTTP/1.1 request parser backed by llhttp.
//
// Pipelining serialization: when a request completes mid-buffer, the parser
// pauses (returns from feed()).  unconsumed() then reports how many trailing
// bytes belong to the next request.  The connection layer buffers them and
// calls resume() + feed() once the current response is fully sent.  Without
// this, two pipelined requests on the same TCP segment would race for the
// connection's write buffer.
class HttpParser {
public:
    using OnComplete = std::function<void(ParsedRequest)>;

    struct ParseContext;

    explicit HttpParser(OnComplete on_complete);
    ~HttpParser();

    // Returns false on a parse error (caller should close the connection).
    // After a successful completed request the parser is left in paused state;
    // call resume() before feeding more bytes.
    bool feed(const char* data, size_t len);

    // True if the most recent feed() stopped because a request completed.
    bool is_paused() const;

    // True if feed() returned false specifically because the body exceeded
    // kMaxBodySize — lets the caller answer 413 instead of a generic 400.
    bool body_too_large() const;

    // Bytes from the most recent feed() that the parser did NOT consume.
    // Only meaningful while paused — they belong to the next pipelined request.
    size_t unconsumed() const;

    // Resume after a pause.  Safe to call when not paused.
    void resume();

    void reset();

private:
    OnComplete                       on_complete_;
    std::unique_ptr<ParseContext>    ctx_;
    std::unique_ptr<llhttp_t>        parser_;
    std::unique_ptr<llhttp_settings_t> settings_;

    // Last buffer passed to feed(); used to compute unconsumed() via the
    // llhttp_get_error_pos pointer.
    const char* last_data_ = nullptr;
    size_t      last_len_  = 0;
};

} // namespace lumen::http
