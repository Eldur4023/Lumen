#pragma once
#include <string>

#include "value.hpp"

namespace lumen {
class Request; class Response;
}

namespace lumen_script {

struct SessionState;
struct NativeCtx;

// ─── Signed session and JWT ──────────────────────────────────────────────────
//
// The session is a signed cookie, like in Flask: no server-side state, which
// fits one VM per request and N event loops with nothing to synchronize.
//
// Format:  exp "." base64url(json) "." base64url(hmac_sha256(secret, exp "." base64url(json)))
//
// `exp` (unix seconds) is signed but kept OUTSIDE the JSON payload, so it
// never shows up as a spurious key when a handler reads `session.*` or
// iterates the session dict — it is envelope, not application data.
//
// Without it, `Max-Age` on the cookie was the ONLY expiry: a client that
// keeps an old Set-Cookie value (or an attacker who steals one) could replay
// it forever, and `session.clear()` / logout only ever told the *browser* to
// drop the cookie — a copy taken before that request stayed valid, HMAC and
// all.  Rejecting on `exp` here closes that: a stolen or retained cookie
// stops working on its own once session_max_age has passed, no rotation of
// the secret required.
//
// The content is signed but NOT encrypted: the user can read it, they just
// cannot forge it.  Nothing they should not see is kept there.
//
// Deliberately a public header: this logic does not depend on bytecode or
// the VM — any backend that runs Lumen Script routes needs it, whether
// today's interpreter or a native-compilation backend (see
// COMPILACION-NATIVA.md, phase 0).

constexpr const char* kSessionCookie = "lumen_session";

std::string sign_session(const Value::Dict& data, const std::string& secret,
                         long long exp);

// Returns false if the cookie is missing, malformed, expired, or the
// signature does not match.  In any of those cases the session starts empty,
// never half-filled.
bool load_session(const std::string& cookie, const std::string& secret,
                  Value::Dict& out);

// Verifies an HS256 JWT and returns the claims.
//
// It checks alg, signature and expiry.  A token with alg "none", or with RS256
// when we expect HS256, is rejected: accepting whatever alg the token names is
// the classic JWT library vulnerability.
bool verify_jwt(const std::string& token, const std::string& secret,
                const std::string& issuer, Value& claims_out);

// Authentication configuration each handler needs at runtime.
struct AuthConfig {
    std::string session_secret;
    int         session_max_age = 86400;
    bool        session_secure  = true;
    std::string jwt_secret;
    std::string jwt_issuer;
};

// Prepares session and claims before running the handler.
void begin_auth(const AuthConfig& cfg, lumen::Request& req,
                SessionState& session, Value& claims, NativeCtx& ctx);

// Rewrites the cookie only if the handler touched the session.
void end_auth(const AuthConfig& cfg, const SessionState& session,
              lumen::Response& res);

} // namespace lumen_script
