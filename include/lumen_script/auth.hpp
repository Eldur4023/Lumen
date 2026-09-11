#pragma once
#include <string>

#include "value.hpp"

namespace lumen {
class Request; class Response;
}

namespace lumen_script {

struct SessionState;
struct NativeCtx;

// ─── Sesion firmada y JWT ────────────────────────────────────────────────────
//
// La sesion es una cookie firmada, como en Flask: sin estado en servidor, lo
// que encaja con un VM por peticion y N event loops sin nada que sincronizar.
//
// Formato:  base64url(json) "." base64url(hmac_sha256(secreto, base64url(json)))
//
// El contenido va firmado pero NO cifrado: el usuario puede leerlo, solo no
// puede falsificarlo.  No se guarda ahi nada que no pueda ver.
//
// Header publico a proposito: esta logica no depende del bytecode ni del VM
// -- la necesita cualquier backend que ejecute rutas de Lumen Script, sea el
// interprete de hoy o un backend de compilacion nativa (ver
// COMPILACION-NATIVA.md, fase 0).

constexpr const char* kSessionCookie = "lumen_session";

std::string sign_session(const Value::Dict& data, const std::string& secret);

// Devuelve false si la cookie falta, esta mal formada o la firma no cuadra.
// En cualquiera de esos casos la sesion arranca vacia, nunca a medias.
bool load_session(const std::string& cookie, const std::string& secret,
                   Value::Dict& out);

// Verifica un JWT HS256 y devuelve los claims.
//
// Comprueba alg, firma y expiracion.  Un token con alg "none", o con RS256
// cuando esperamos HS256, se rechaza: aceptar el alg que diga el token es la
// vulnerabilidad clasica de las librerias de JWT.
bool verify_jwt(const std::string& token, const std::string& secret,
                 const std::string& issuer, Value& claims_out);

// Configuracion de autenticacion que cada handler necesita en runtime.
struct AuthConfig {
    std::string session_secret;
    int         session_max_age = 86400;
    bool        session_secure  = true;
    std::string jwt_secret;
    std::string jwt_issuer;
};

// Prepara sesion y claims antes de ejecutar el handler.
void begin_auth(const AuthConfig& cfg, lumen::Request& req,
                 SessionState& session, Value& claims, NativeCtx& ctx);

// Reescribe la cookie solo si el handler toco la sesion.
void end_auth(const AuthConfig& cfg, const SessionState& session,
               lumen::Response& res);

} // namespace lumen_script
