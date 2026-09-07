// Herramienta de desarrollo: comprueba SHA-256, HMAC-SHA256 y base64url.
//
//   crypto_check sha  <texto>
//   crypto_check hmac <clave> <texto>
//   crypto_check b64  <texto>
//   crypto_check self          → vectores conocidos
#include <lumen_script/crypto.hpp>
#include <iostream>
#include <string>

namespace {

std::string hex(const std::string& raw) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) { out += d[c >> 4]; out += d[c & 0xF]; }
    return out;
}

int failures = 0;

void expect(const std::string& what, const std::string& got, const std::string& want) {
    bool ok = (got == want);
    if (!ok) ++failures;
    std::cout << (ok ? "  ok   " : "  FALLO ") << what << "\n";
    if (!ok) std::cout << "        obtenido: " << got << "\n"
                       << "        esperado: " << want << "\n";
}

int self_test() {
    using namespace lumen_script::crypto;

    expect("sha256(\"\")", hex(sha256("")),
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect("sha256(\"abc\")", hex(sha256("abc")),
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect("sha256(56 bytes)",
           hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // Cruza el limite de bloque en el relleno (55, 56 y 64 bytes son los casos
    // que rompen una implementacion mal hecha).
    expect("sha256(55 x 'a')", hex(sha256(std::string(55, 'a'))),
           "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    expect("sha256(56 x 'a')", hex(sha256(std::string(56, 'a'))),
           "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    expect("sha256(64 x 'a')", hex(sha256(std::string(64, 'a'))),
           "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");

    // RFC 4231, caso 2.
    expect("hmac(\"Jefe\", ...)",
           hex(hmac_sha256("Jefe", "what do ya want for nothing?")),
           "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // Clave mas larga que el bloque: se sustituye por su hash (RFC 2104).
    expect("hmac(clave de 131 bytes)",
           hex(hmac_sha256(std::string(131, '\xaa'),
                           "Test Using Larger Than Block-Size Key - Hash Key First")),
           "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    std::string round;
    const std::string raw = std::string("\x00\x01\xfe\xff", 4) + "lumen_script";
    expect("base64url ida y vuelta",
           base64url_decode(base64url_encode(raw), round) && round == raw ? "si" : "no",
           "si");
    expect("base64url sin relleno", base64url_encode("a"), "YQ");
    expect("base64url alfabeto url-safe",
           base64url_encode(std::string("\xfb\xff", 2)), "-_8");

    std::cout << (failures ? "\nFALLOS: " : "\ntodo correcto (")
              << (failures ? std::to_string(failures) : std::string("11"))
              << (failures ? "" : " comprobaciones)") << "\n";
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "uso: crypto_check self|sha|hmac|b64 ...\n"; return 2; }
    std::string mode = argv[1];

    if (mode == "self") return self_test();
    if (mode == "sha"  && argc > 2) { std::cout << hex(lumen_script::crypto::sha256(argv[2])) << "\n"; return 0; }
    if (mode == "hmac" && argc > 3) { std::cout << hex(lumen_script::crypto::hmac_sha256(argv[2], argv[3])) << "\n"; return 0; }
    if (mode == "b64"  && argc > 2) { std::cout << lumen_script::crypto::base64url_encode(argv[2]) << "\n"; return 0; }

    std::cerr << "argumentos invalidos\n";
    return 2;
}
