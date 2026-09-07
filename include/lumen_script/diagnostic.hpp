#pragma once
#include <string>
#include <vector>
#include "token.hpp"

namespace lumen_script {

// A source file already read.  `path` lives here and the SourceLocs point at
// it, so a SourceFile cannot move while there are live tokens.
struct SourceFile {
    std::string path;
    std::string text;

    // Returns line `n` (1-indexed) without the trailing break.
    std::string_view line(int n) const;
};

struct Diagnostic {
    SourceLoc   loc;
    std::string message;
};

// Collects errors instead of aborting on the first: a file with three faults
// should report all three, not force compiling three times.
class DiagnosticBag {
public:
    void error(SourceLoc loc, std::string message) {
        items_.push_back({loc, std::move(message)});
    }

    bool empty()  const { return items_.empty(); }
    size_t size() const { return items_.size(); }
    const std::vector<Diagnostic>& items() const { return items_; }

    void clear() { items_.clear(); }

    // Formats every diagnostic with file:line:column, the line of code and a
    // cursor under the exact position.  `files` is used to recover the text of
    // each line.
    std::string format(const std::vector<const SourceFile*>& files) const;

private:
    std::vector<Diagnostic> items_;
};

} // namespace lumen_script
