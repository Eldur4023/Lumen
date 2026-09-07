#pragma once
#include <string>
#include <string_view>

namespace lumen_script::crypto {

// SHA-256 y HMAC-SHA256 propios, sin OpenSSL.
//
// Lumen 2.0 no enlaza OpenSSL —TLS es cosa del reverse proxy— pero sigue
// necesitando un MAC para firmar la cookie de sesion y los JWT HS256.  Son las
// dos unicas operaciones criptograficas del proyecto, y ambas caben aqui.
//
// Esto NO es un sustituto de una biblioteca criptografica: solo implementa lo
// que hace falta, y nada mas.

// Devuelve los 32 bytes crudos del hash.
std::string sha256(std::string_view data);

// Devuelve los 32 bytes crudos del MAC (RFC 2104).
std::string hmac_sha256(std::string_view key, std::string_view message);

// Base64 con el alfabeto URL-safe y sin relleno, como exige JWT (RFC 7515).
std::string base64url_encode(std::string_view raw);
bool        base64url_decode(std::string_view text, std::string& out);

// Base64 del alfabeto estandar, con relleno (RFC 4648).  Es como se mete un
// binario en un JSON: los modulos de base de datos la usan para las columnas
// que no son texto, porque un blob crudo en la respuesta la dejaria sin ser
// UTF-8 valido y ningun cliente sabria leerla.
std::string base64_encode(std::string_view raw);

// Comparacion en tiempo constante.  Comparar firmas con == filtra por el
// tiempo de respuesta cuantos bytes iniciales acerto el atacante, que es
// suficiente para reconstruir la firma byte a byte.
bool constant_time_equal(std::string_view a, std::string_view b);

// Bytes aleatorios de /dev/urandom.  Devuelve una cadena vacia si no se
// pueden obtener, y quien llama debe tratarlo como fallo, nunca continuar con
// un valor predecible.
std::string random_bytes(size_t n);

} // namespace lumen_script::crypto
