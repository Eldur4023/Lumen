#include "../include/lumen/router.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace lumen {

// ─────────────────────────────────────────────────────────────────────────────
// Node Helpers
// ─────────────────────────────────────────────────────────────────────────────

Router::Node* Router::Node::find_static_child(const std::string& seg) const {
    for (auto& child : children) {
        if (child->type == NodeType::STATIC && child->segment == seg) return child.get();
    }
    return nullptr;
}

Router::Node* Router::Node::find_param_child() const {
    for (auto& child : children) {
        if (child->type == NodeType::PARAM) return child.get();
    }
    return nullptr;
}

Router::Node* Router::Node::find_wildcard_child() const {
    for (auto& child : children) {
        if (child->type == NodeType::WILDCARD) return child.get();
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Router
// ─────────────────────────────────────────────────────────────────────────────

Router::Router() : root_(std::make_unique<Node>()) {
    root_->segment = "";
    root_->type = NodeType::STATIC;
}

std::string Router::normalize_pattern(const std::string& p) {
    std::string out;
    out.reserve(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '{') {
            out += ':';
            ++i;
            bool closed = false;
            while (i < p.size()) {
                if (p[i] == '}') { closed = true; break; }
                out += p[i++];
            }
            if (!closed) {
                // Unclosed brace: the pattern is malformed.  Better to fail
                // loudly at registration than silently match weird URLs.
                throw std::invalid_argument(
                    "lumen::Router: unterminated '{' in pattern: " + p);
            }
        } else {
            out += p[i];
        }
    }
    if (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

static std::vector<std::string> split_path(const std::string& s) {
    std::vector<std::string> parts;
    std::istringstream ss(s);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (!seg.empty()) parts.push_back(seg);
    }
    return parts;
}

// Percent-decodes one path segment bound to a `:param`.  Static segments are
// matched as literal text on purpose (that is what makes route registration
// predictable), so this is applied ONLY to the value handed to a handler —
// never to the segments used for matching, which stay exactly as received.
//
// Without it, `GET /echo/%34%32` (which is "42") bound `id` to the literal
// text "%34%32" instead of "42": every consumer downstream (int/float
// coercion, a string param compared against a literal) saw percent-escapes
// no client-facing route documents, while query() and form() — which already
// decode — did not have the same problem.  No '+' -> ' ' folding here: that
// convention belongs to application/x-www-form-urlencoded bodies and query
// strings, not path segments.
static std::string url_decode_segment(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char buf[3] = {s[i + 1], s[i + 2], '\0'};
            char* end = nullptr;
            unsigned long v = std::strtoul(buf, &end, 16);
            if (end == buf + 2) {
                // Drop %00: a NUL bound into a param could desync a later
                // C-string-based use of it from what the router matched on.
                if (v != 0) out += static_cast<char>(v);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

void Router::add_internal(std::string method, std::string pattern, Handler handler) {
    std::transform(method.begin(), method.end(), method.begin(), ::toupper);
    std::string norm = normalize_pattern(pattern);
    auto segments = split_path(norm);

    Node* curr = root_.get();
    for (const auto& seg : segments) {
        NodeType type = NodeType::STATIC;
        std::string name = seg;

        if (seg == "*") {
            type = NodeType::WILDCARD;
        } else if (seg[0] == ':') {
            type = NodeType::PARAM;
            name = seg.substr(1);
        }

        Node* next = nullptr;
        if (type == NodeType::STATIC) next = curr->find_static_child(name);
        else if (type == NodeType::PARAM) next = curr->find_param_child();
        else next = curr->find_wildcard_child();

        if (!next) {
            auto node = std::make_unique<Node>();
            node->segment = name;
            node->type = type;
            next = node.get();
            curr->children.push_back(std::move(node));
        }
        curr = next;
    }
    curr->handlers[method] = std::move(handler);
}

RouteMatch Router::match(const std::string& method, const std::string& path) const {
    std::string clean_path = path;
    if (clean_path.size() > 1 && clean_path.back() == '/') clean_path.pop_back();

    auto segments = split_path(clean_path);
    std::unordered_map<std::string, std::string> params;
    Handler handler = nullptr;

    if (match_recursive(root_.get(), segments, 0, method, params, handler)) {
        return {true, handler, std::move(params)};
    }
    return {false, nullptr, {}};
}

bool Router::match_recursive(
    const Node* node,
    const std::vector<std::string>& segments,
    size_t index,
    const std::string& method,
    std::unordered_map<std::string, std::string>& params,
    Handler& out_handler) const 
{
    // Terminal case
    if (index == segments.size()) {
        auto it = node->handlers.find(method);
        if (it != node->handlers.end()) {
            out_handler = it->second;
            return true;
        }
        // Check wildcard handle anyway (*)
        it = node->handlers.find("*");
        if (it != node->handlers.end()) {
            out_handler = it->second;
            return true;
        }
        return false;
    }

    const std::string& seg = segments[index];

    // 1. Try static
    Node* next = node->find_static_child(seg);
    if (next && match_recursive(next, segments, index + 1, method, params, out_handler)) {
        return true;
    }

    // 2. Try param
    next = node->find_param_child();
    if (next) {
        params[next->segment] = url_decode_segment(seg);
        if (match_recursive(next, segments, index + 1, method, params, out_handler)) {
            return true;
        }
        params.erase(next->segment); // backtrack
    }

    // 3. Try wildcard
    next = node->find_wildcard_child();
    if (next) {
        // Wildcard matches EVERYTHING remaining
        auto it = next->handlers.find(method);
        if (it == next->handlers.end()) it = next->handlers.find("*");
        
        if (it != next->handlers.end()) {
            out_handler = it->second;
            // Build the rest of the path for the wildcard if needed?
            // Usually wildcard just captures the rest.
            return true;
        }
    }

    return false;
}

} // namespace lumen
