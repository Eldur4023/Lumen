#include <lumen_script/auth.hpp>

#include <lumen_script/crypto.hpp>
#include <lumen_script/natives.hpp>

#include <lumen/cookies.hpp>
#include <lumen/request.hpp>
#include <lumen/response.hpp>

#include <ctime>

namespace lumen_script {

std::string sign_session(const Value::Dict& data, const std::string& secret,
                         long long exp) {
    std::string exp_str = std::to_string(exp);
    std::string payload = crypto::base64url_encode(
        Value::dict(data).to_json_text());
    std::string signing_input = exp_str + "." + payload;
    std::string mac = crypto::base64url_encode(
        crypto::hmac_sha256(secret, signing_input));
    return signing_input + "." + mac;
}

bool load_session(const std::string& cookie, const std::string& secret,
                  Value::Dict& out) {
    size_t dot2 = cookie.rfind('.');
    if (dot2 == std::string::npos) return false;
    size_t dot1 = cookie.rfind('.', dot2 - 1);
    if (dot1 == std::string::npos) return false;

    std::string exp_str = cookie.substr(0, dot1);
    std::string payload = cookie.substr(dot1 + 1, dot2 - dot1 - 1);
    std::string given   = cookie.substr(dot2 + 1);

    std::string signing_input = exp_str + "." + payload;
    std::string expected = crypto::base64url_encode(
        crypto::hmac_sha256(secret, signing_input));
    if (!crypto::constant_time_equal(given, expected)) return false;

    // The MAC just verified proves this cookie was minted by us with this
    // exact exp_str, so branching on its value now leaks nothing an attacker
    // could use — they would need a valid signature to get here at all.
    long long exp = 0;
    try {
        size_t consumed = 0;
        exp = std::stoll(exp_str, &consumed);
        if (consumed != exp_str.size()) return false;
    } catch (...) {
        return false;
    }
    const long long now = static_cast<long long>(std::time(nullptr));
    if (now >= exp) return false;

    std::string json_text;
    if (!crypto::base64url_decode(payload, json_text)) return false;

    Value v;
    if (!Value::parse_json(json_text, v) || !v.is_dict()) return false;
    out = v.as_dict();
    return true;
}

bool verify_jwt(const std::string& token, const std::string& secret,
                const std::string& issuer, Value& claims_out) {
    size_t p1 = token.find('.');
    if (p1 == std::string::npos) return false;
    size_t p2 = token.find('.', p1 + 1);
    if (p2 == std::string::npos) return false;

    std::string signing_input = token.substr(0, p2);
    std::string given_sig     = token.substr(p2 + 1);

    std::string expected = crypto::base64url_encode(
        crypto::hmac_sha256(secret, signing_input));
    if (!crypto::constant_time_equal(given_sig, expected)) return false;

    std::string header_text, payload_text;
    if (!crypto::base64url_decode(token.substr(0, p1), header_text)) return false;
    if (!crypto::base64url_decode(token.substr(p1 + 1, p2 - p1 - 1), payload_text))
        return false;

    Value header;
    if (!Value::parse_json(header_text, header) || !header.is_dict()) return false;
    {
        auto it = header.as_dict().find("alg");
        if (it == header.as_dict().end() || !it->second.is_str() ||
            it->second.as_str() != "HS256")
            return false;
    }

    Value payload;
    if (!Value::parse_json(payload_text, payload) || !payload.is_dict()) return false;

    if (auto it = payload.as_dict().find("exp");
        it != payload.as_dict().end() && it->second.is_num()) {
        const long long now = static_cast<long long>(std::time(nullptr));
        if (static_cast<long long>(it->second.as_float()) < now) return false;
    }
    if (!issuer.empty()) {
        auto it = payload.as_dict().find("iss");
        if (it != payload.as_dict().end() &&
            (!it->second.is_str() || it->second.as_str() != issuer))
            return false;
    }

    claims_out = std::move(payload);
    return true;
}

void begin_auth(const AuthConfig& cfg, lumen::Request& req,
                SessionState& session, Value& claims, NativeCtx& ctx) {
    session.secret = cfg.session_secret;
    if (!cfg.session_secret.empty()) {
        auto cookie = req.cookie(kSessionCookie);
        if (cookie) load_session(*cookie, cfg.session_secret, session.data);
        session.loaded = true;
    }
    ctx.session = &session;

    if (!cfg.jwt_secret.empty()) {
        auto auth = req.header("authorization");
        if (auth && auth->rfind("Bearer ", 0) == 0) {
            ctx.jwt_ok = verify_jwt(auth->substr(7), cfg.jwt_secret,
                                    cfg.jwt_issuer, claims);
        }
    }
    ctx.jwt_claims = &claims;
}

void end_auth(const AuthConfig& cfg, const SessionState& session,
              lumen::Response& res) {
    if (!session.dirty || cfg.session_secret.empty()) return;

    lumen::CookieOptions opts;
    opts.path      = "/";
    opts.http_only = true;                 // JS cannot read it
    opts.secure    = cfg.session_secure;
    opts.same_site = lumen::SameSite::Lax;

    if (session.data.empty()) {
        res.clear_cookie(kSessionCookie, opts);
        return;
    }
    opts.max_age = cfg.session_max_age;
    const long long exp = static_cast<long long>(std::time(nullptr)) + cfg.session_max_age;
    res.cookie(kSessionCookie, sign_session(session.data, cfg.session_secret, exp), opts);
}

} // namespace lumen_script
