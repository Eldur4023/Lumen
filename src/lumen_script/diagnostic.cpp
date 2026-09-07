#include <lumen_script/diagnostic.hpp>
#include <algorithm>

namespace lumen_script {

std::string_view SourceFile::line(int n) const {
    if (n < 1) return {};
    size_t start = 0;
    int    cur   = 1;
    while (cur < n) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) return {};
        start = nl + 1;
        ++cur;
    }
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    // Strips the \r from Windows line endings.
    if (end > start && text[end - 1] == '\r') --end;
    return std::string_view(text).substr(start, end - start);
}

std::string DiagnosticBag::format(const std::vector<const SourceFile*>& files) const {
    std::string out;
    for (const auto& d : items_) {
        const SourceFile* src = nullptr;
        if (d.loc.file) {
            for (const auto* f : files) {
                if (&f->path == d.loc.file) { src = f; break; }
            }
        }

        const std::string& path = d.loc.file ? *d.loc.file : std::string();
        out += path;
        out += ":" + std::to_string(d.loc.line) + ":" + std::to_string(d.loc.col);
        out += ": error: " + d.message + "\n";

        if (src) {
            std::string_view text = src->line(d.loc.line);
            if (!text.empty()) {
                std::string num = std::to_string(d.loc.line);
                out += "  " + num + " | ";
                out += std::string(text) + "\n";
                // The cursor goes under the column; the tabs of the source are
                // reproduced so it stays aligned.
                out += "  " + std::string(num.size(), ' ') + " | ";
                for (int i = 0; i + 1 < d.loc.col && i < (int)text.size(); ++i)
                    out += (text[i] == '\t') ? '\t' : ' ';
                out += "^\n";
            }
        }
    }
    return out;
}

} // namespace lumen_script
