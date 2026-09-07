#pragma once
#include <cstdio>
#include <string>

namespace lumen {

// La pagina de Swagger UI, y nada mas.
//
// El documento OpenAPI ya no se deduce de las firmas de C++ con plantillas: lo
// genera el frontend de Lumen Script a partir del AST del .lum, que conoce los nombres
// de los parametros y las clases.  De este fichero solo sobrevive el HTML que
// sirve la UI contra ese documento.

// ─── Swagger UI HTML (CDN) ───────────────────────────────────────────────────
// Serves a self-contained Swagger UI page from unpkg CDN.
// For air-gapped deployments: embed swagger-ui-dist assets instead.

inline std::string swagger_ui_html(const std::string& spec_url = "/openapi.json") {
    // Defence-in-depth: spec_url is normally a hard-coded route, but any path
    // that ends up here gets escaped as a JS string literal so a misconfigured
    // caller (e.g. enable_docs(req.query["x"])) cannot inject script.
    std::string escaped;
    escaped.reserve(spec_url.size() + 8);
    for (char c : spec_url) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"':  escaped += "\\\""; break;
            case '\'': escaped += "\\u0027"; break;
            case '<':  escaped += "\\u003c"; break;
            case '>':  escaped += "\\u003e"; break;
            case '&':  escaped += "\\u0026"; break;
            case '\r': escaped += "\\r"; break;
            case '\n': escaped += "\\n"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    escaped += b;
                } else {
                    escaped += c;
                }
        }
    }

    return R"(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8"/>
  <title>Lumen — API Docs</title>
  <link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css">
  <style>body { margin: 0; }</style>
</head>
<body>
  <div id="swagger-ui"></div>
  <script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
  <script>
    SwaggerUIBundle({
      url: ")" + escaped + R"(",
      dom_id: '#swagger-ui',
      deepLinking: true,
      presets: [SwaggerUIBundle.presets.apis, SwaggerUIBundle.SwaggerUIStandalonePreset],
      layout: "StandaloneLayout"
    });
  </script>
</body>
</html>)";
}

} // namespace lumen
