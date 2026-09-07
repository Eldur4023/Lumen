#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <map>

#include <lumen/router.hpp>
#include "bytecode.hpp"
#include "ast.hpp"
#include "template.hpp"
#include "diagnostic.hpp"

namespace lumen_script {

// A compiled module: the result of reading a set of .lum files.
//
// It is the unit that gets hot-swapped.  Reloading is building a new Module
// and publishing the shared_ptr; if compilation fails, the previous one stays
// in place and nothing is touched.
struct Module {
    std::vector<std::unique_ptr<SourceFile>> files;
    Program        program;
    lumen::Router router;

    // Compiled user functions, indexed by declaration order.
    FunctionTable functions;

    // Compiled templates, indexed by the order the emitter found them in.
    // Every render() in the source has its own, compiled against the specific
    // keys that call passes it.
    std::vector<Template> templates;

    // OpenAPI specification generated from the AST at compile time.
    std::string    openapi;      // already serialized at compile time

    // `on error` handlers, by code.  Key 0 is the global one.
    std::map<int, std::shared_ptr<Chunk>> error_handlers;

    // Split between the two route levels: the declarative ones do not run a
    // single bytecode step.
    int declarative_routes = 0;
    int vm_routes          = 0;

    // mtimes of the compiled files, to detect changes.
    std::vector<std::pair<std::filesystem::path,
                          std::filesystem::file_time_type>> stamps;
};

// Resolves the command line arguments to the set of files to compile:
// compilar:
//   • one file      → just that one
//   • several       → just those
//   • a directory   → every .lum inside it, recursively
//
// Returns false and writes to `error` if an argument is missing or if a
// directorio no contiene ningun .lum.
bool resolve_inputs(const std::vector<std::string>& args,
                    std::vector<std::filesystem::path>& out,
                    std::string& error);

// Reads, lexes, parses and builds the route table.
//
// It ALWAYS returns a Module, even if there were errors: the SourceLocs point
// at the SourceFiles this Module owns, so destroying it before formatting the
// diagnostics would leave dangling pointers.  Success is checked with
// `diags.empty()`, and a module with errors is simply not published.
std::shared_ptr<Module> compile(const std::vector<std::filesystem::path>& inputs,
                                DiagnosticBag& diags);

// Formats the diagnostics of a failed attempt using the files that were read.
std::string format_errors(const DiagnosticBag& diags,
                          const std::vector<std::unique_ptr<SourceFile>>& files);

} // namespace lumen_script
