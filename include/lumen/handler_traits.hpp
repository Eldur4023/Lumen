#pragma once
#include <tuple>
#include <type_traits>
#include <utility>

#include "request.hpp"
#include "response.hpp"
#include "task.hpp"

namespace lumen {

// Adapta un handler de C++ a la firma que guarda el router.
//
// En Lumen 2.0 los handlers nativos son un punado —/docs, /health, /metrics y
// el dispatcher de Lumen Script—, asi que la adaptacion se reduce a repartir `Request&`
// y `Response&` y a aceptar tanto `void` como `Task<void>`.
//
// La extraccion tipada de parametros —ruta, query, cuerpo, validacion,
// inyeccion— vivia aqui como metaprogramacion de plantillas.  Ahora la resuelve
// el frontend de Lumen Script al compilar el .lum, con los nombres y los tipos
// delante, asi que aquello sobra.

namespace detail {

template<typename T> struct handler_arg;
template<> struct handler_arg<Request&> {
    static Request& get(Request& req, Response&) { return req; }
};
template<> struct handler_arg<const Request&> {
    static const Request& get(Request& req, Response&) { return req; }
};
template<> struct handler_arg<Response&> {
    static Response& get(Request&, Response& res) { return res; }
};

template<typename T> struct callable_traits;
template<typename C, typename R, typename... A>
struct callable_traits<R (C::*)(A...) const> {
    using result = R;
    using args   = std::tuple<A...>;
};
template<typename C, typename R, typename... A>
struct callable_traits<R (C::*)(A...)> {
    using result = R;
    using args   = std::tuple<A...>;
};

} // namespace detail

template<typename F>
struct HandlerTraits {
private:
    using traits = detail::callable_traits<decltype(&std::decay_t<F>::operator())>;

    template<typename... A>
    static Task<void> invoke(F& f, Request& req, Response& res, std::tuple<A...>*) {
        if constexpr (std::is_same_v<typename traits::result, Task<void>>)
            co_await f(detail::handler_arg<A>::get(req, res)...);
        else
            f(detail::handler_arg<A>::get(req, res)...);
        co_return;
    }

public:
    static Task<void> call(F& f, Request& req, Response& res) {
        return invoke(f, req, res, static_cast<typename traits::args*>(nullptr));
    }
};

} // namespace lumen
