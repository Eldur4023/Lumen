#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>
#include "types.hpp"
#include "handler_traits.hpp"

namespace lumen {

struct RouteMatch {
    bool    found   = false;
    Handler handler = nullptr;
    std::unordered_map<std::string, std::string> params;
};

class Router {
public:
    Router();

    // Register any callable; HandlerTraits wraps it into Handler (Task<void>).
    template<typename F>
    void add(std::string method, std::string pattern, F&& handler) {
        auto wrapped = [h = std::forward<F>(handler)](Request& req, Response& res) mutable -> Task<void> {
            return HandlerTraits<std::decay_t<F>>::call(h, req, res);
        };
        add_internal(std::move(method), std::move(pattern), std::move(wrapped));
    }

    void add_internal(std::string method, std::string pattern, Handler handler);

    RouteMatch match(const std::string& method, const std::string& path) const;

private:
    enum class NodeType { STATIC, PARAM, WILDCARD };

    struct Node {
        std::string segment;
        NodeType    type = NodeType::STATIC;
        std::unordered_map<std::string, Handler> handlers; // method -> handler
        std::vector<std::unique_ptr<Node>> children;

        Node* find_static_child(const std::string& seg) const;
        Node* find_param_child() const;
        Node* find_wildcard_child() const;
    };

    std::unique_ptr<Node> root_;

    static std::string normalize_pattern(const std::string& p);

    bool match_recursive(
        const Node* node,
        const std::vector<std::string>& segments,
        size_t index,
        const std::string& method,
        std::unordered_map<std::string, std::string>& params,
        Handler& out_handler) const;
};

} // namespace lumen
