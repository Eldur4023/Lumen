#pragma once
#include <memory>
#include <string>

#include "project.hpp"

namespace lumen_script {

// Automatic endpoint probe.
//
// After startup and after every successful reload, Lumen talks to itself over
// HTTP and walks the module's routes.  It does not check business logic: what
// it looks for is that no handler broke —a 5xx— after a change.
//
// On side effects: probing an endpoint RUNS its handler.  A DELETE or a POST
// would really do their work on every reload, so by default only the safe
// methods (GET and HEAD) are walked.  The rest come in with `--autotest=all`,
// which is a decision of whoever launches it and not of the binary.
struct AutotestOptions {
    bool     enabled       = false;
    bool     unsafe        = false;   // incluir POST/PUT/PATCH/DELETE
    uint16_t port          = 8080;
    int      timeout_ms    = 3000;
    int      stream_ms     = 400;     // how long an sse route is listened to
};

// Walks the module's routes and prints the result.  It blocks, so it is called
// from a thread separate from the event loop.
void run_autotest(const Module& mod, const AutotestOptions& opts);

} // namespace lumen_script
