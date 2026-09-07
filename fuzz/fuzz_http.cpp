// Fuzzing harness for the HTTP parser (llhttp involved) and the multipart
// parser.  The seeds are valid hand-written requests; they are mutated in
// bites and fed to the real parser, in process, with no socket involved.
//
//   fuzz_http [iterations]

#include "chaos.hpp"
#include <lumen/multipart.hpp>
#include <lumen/request.hpp>

// http_parser.hpp is internal to the lumen library (it lives in src/, not in
// include/): it is included directly here, as tests/placeholders.cpp already
// does with the postgres driver, to test THE code and not a summarized copy.
#include "../src/http/http_parser.hpp"

#include <cstdlib>
#include <string>
#include <vector>

using namespace lumen;

static const std::vector<std::string> kHttpSeeds = {
    "GET /articulos/42 HTTP/1.1\r\nHost: x\r\n\r\n",
    "POST /articulos HTTP/1.1\r\nHost: x\r\nContent-Length: 16\r\n"
    "Content-Type: application/json\r\n\r\n{\"title\":\"hi\"}",
    "GET /buscar?q=algo&page=3 HTTP/1.1\r\nHost: x\r\nX-Prueba: value\r\n"
    "Connection: keep-alive\r\n\r\n",
    "PUT /x HTTP/1.0\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n0\r\n\r\n",
    "DELETE /u/1 HTTP/1.1\r\nHost: x\r\nCookie: a=1; b=2\r\nIf-None-Match: \"x\"\r\n\r\n",
};

static const std::vector<std::string> kMultipartSeeds = {
    "------limite123\r\n"
    "Content-Disposition: form-data; name=\"title\"\r\n\r\n"
    "hola\r\n"
    "------limite123\r\n"
    "Content-Disposition: form-data; name=\"f\"; filename=\"a.txt\"\r\n"
    "Content-Type: text/plain\r\n\r\n"
    "file contents\r\n"
    "------limite123--\r\n",
};

int main(int argc, char** argv) {
    int iterations = argc > 1 ? std::atoi(argv[1]) : 20000;

    int f1 = chaos::run("http_parser", iterations, 2000, kHttpSeeds,
                            [](const std::string& case_) {
                                http::HttpParser p([](http::ParsedRequest) {});
                                p.feed(case_.data(), case_.size());
                            });

    int f2 = chaos::run("multipart", iterations, 2000, kMultipartSeeds,
                            [](const std::string& case_) {
                                Request req;
                                req.method = "POST";
                                req.headers["content-type"] =
                                    "multipart/form-data; boundary=----limite123";
                                req.body = case_;
                                auto parts = parse_multipart(req);
                                (void)parts;
                            });

    return (f1 || f2) ? 1 : 0;
}
