#pragma once
#include "http.h"
#include <functional>
#include <vector>
#include <string>
#include <unordered_map>

using Handler = std::function<HttpResponse(const HttpRequest&)>;
using Middleware = std::function<bool(const HttpRequest&, HttpResponse&)>;

struct Route {
    std::string method;
    std::string path;
    Handler handler;
};

class Router {
public:
    void add_route(const std::string& method, const std::string& path, Handler h) {
        routes_.push_back({method, path, h});
    }

    void use_middleware(Middleware mw) {
        middlewares_.push_back(mw);
    }

    HttpResponse handle(const HttpRequest& req) const {
        // Run middlewares
        HttpResponse early;
        for (auto& mw : middlewares_) {
            if (!mw(req, early)) return early;  // middleware rejected
        }

        // Match route
        for (auto& route : routes_) {
            if (route.method == req.method && route.path == req.path) {
                try {
                    return route.handler(req);
                } catch (const std::exception& e) {
                    return HttpResponse::make_error(500, e.what());
                }
            }
        }

        // Check method mismatch
        for (auto& route : routes_) {
            if (route.path == req.path)
                return HttpResponse::make_error(405, "Method Not Allowed");
        }

        return HttpResponse::make_error(404, "Route not found: " + req.path);
    }

private:
    std::vector<Route> routes_;
    std::vector<Middleware> middlewares_;
};
