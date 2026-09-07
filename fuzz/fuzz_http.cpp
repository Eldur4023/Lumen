// Arnes de fuzzing para el parser HTTP (llhttp de por medio) y el parser
// multipart. Las semillas son peticiones validas escritas a mano; se mutan a
// bocados y se alimentan al parser real, en proceso, sin socket de por medio.
//
//   fuzz_http [iteraciones]

#include "chaos.hpp"
#include <lumen/multipart.hpp>
#include <lumen/request.hpp>

// http_parser.hpp es interno a la biblioteca lumen (vive en src/, no en
// include/): aqui se incluye directo, como ya hace tests/marcadores.cpp con el
// driver de postgres, para probar EL codigo y no una copia resumida de el.
#include "../src/http/http_parser.hpp"

#include <cstdlib>
#include <string>
#include <vector>

using namespace lumen;

static const std::vector<std::string> kSemillasHttp = {
    "GET /articulos/42 HTTP/1.1\r\nHost: x\r\n\r\n",
    "POST /articulos HTTP/1.1\r\nHost: x\r\nContent-Length: 16\r\n"
    "Content-Type: application/json\r\n\r\n{\"titulo\":\"hi\"}",
    "GET /buscar?q=algo&page=3 HTTP/1.1\r\nHost: x\r\nX-Prueba: valor\r\n"
    "Connection: keep-alive\r\n\r\n",
    "PUT /x HTTP/1.0\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n0\r\n\r\n",
    "DELETE /u/1 HTTP/1.1\r\nHost: x\r\nCookie: a=1; b=2\r\nIf-None-Match: \"x\"\r\n\r\n",
};

static const std::vector<std::string> kSemillasMultipart = {
    "------limite123\r\n"
    "Content-Disposition: form-data; name=\"titulo\"\r\n\r\n"
    "hola\r\n"
    "------limite123\r\n"
    "Content-Disposition: form-data; name=\"f\"; filename=\"a.txt\"\r\n"
    "Content-Type: text/plain\r\n\r\n"
    "contenido del fichero\r\n"
    "------limite123--\r\n",
};

int main(int argc, char** argv) {
    int iteraciones = argc > 1 ? std::atoi(argv[1]) : 20000;

    int f1 = chaos::correr("http_parser", iteraciones, 2000, kSemillasHttp,
                            [](const std::string& caso) {
                                http::HttpParser p([](http::ParsedRequest) {});
                                p.feed(caso.data(), caso.size());
                            });

    int f2 = chaos::correr("multipart", iteraciones, 2000, kSemillasMultipart,
                            [](const std::string& caso) {
                                Request req;
                                req.method = "POST";
                                req.headers["content-type"] =
                                    "multipart/form-data; boundary=----limite123";
                                req.body = caso;
                                auto partes = parse_multipart(req);
                                (void)partes;
                            });

    return (f1 || f2) ? 1 : 0;
}
