#include "server.h"
#include "router.h"
#include "gateway.h"

#include <iostream>
#include <csignal>
#include <sstream>
#include <chrono>

// ─── Global server pointer for signal handling ───
Server* g_server = nullptr;
void signal_handler(int) {
    std::cout << "\n[INFO ] Shutting down gracefully...\n";
    if (g_server) {
        g_server->print_stats();
        g_server->stop();
    }
}

// ─── Utility: simple JSON builder ───────────────
std::string json_kv(std::initializer_list<std::pair<std::string,std::string>> kvs) {
    std::ostringstream o;
    o << "{";
    bool first = true;
    for (auto& [k,v] : kvs) {
        if (!first) o << ",";
        o << "\"" << k << "\":\"" << v << "\"";
        first = false;
    }
    o << "}";
    return o.str();
}

// ─── Register all application routes ────────────
void register_routes(Router& router) {

    // ── Health / root ──────────────────────────
    router.add_route("GET", "/", [](const HttpRequest&) {
        return HttpResponse::make_json(200,
            json_kv({{"service","Multithreaded Web Server"},
                     {"status", "running"},
                     {"version","1.0.0"}}));
    });

    router.add_route("GET", "/health", [](const HttpRequest&) {
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return HttpResponse::make_json(200,
            json_kv({{"status","healthy"},
                     {"uptime_epoch", std::to_string(uptime)}}));
    });

    // ── Auth ───────────────────────────────────
    router.add_route("POST", "/login", [](const HttpRequest& req) {
        // In production: validate credentials from DB
        // Body expected: {"username":"alice","password":"secret"}
        auto user_pos = req.body.find("\"username\":\"");
        std::string user = "anonymous";
        if (user_pos != std::string::npos) {
            auto start = user_pos + 12;
            auto end   = req.body.find("\"", start);
            if (end != std::string::npos)
                user = req.body.substr(start, end - start);
        }
        auto token = TokenValidator::generate(user);
        return HttpResponse::make_json(200,
            "{\"token\":\"" + token + "\","
            "\"expires_in\":3600,"
            "\"user\":\"" + user + "\"}");
    });

    // ── Users API ──────────────────────────────
    router.add_route("GET", "/api/users", [](const HttpRequest&) {
        // Stateless: fetch user list from DB (simulated)
        return HttpResponse::make_json(200,
            R"({"users":[)"
            R"({"id":1,"name":"Alice","role":"admin"},)"
            R"({"id":2,"name":"Bob","role":"viewer"})"
            R"(],"total":2})");
    });

    router.add_route("POST", "/api/users", [](const HttpRequest& req) {
        if (req.body.empty())
            return HttpResponse::make_error(400, "Request body required");
        // Simulate creation (in production: INSERT into DB, return new ID)
        return HttpResponse::make_json(201,
            "{\"message\":\"User created\",\"body\":" + req.body + "}",
            "Created");
    });

    // ── Echo / debug ───────────────────────────
    router.add_route("POST", "/api/echo", [](const HttpRequest& req) {
        std::ostringstream o;
        o << "{\"method\":\"" << req.method << "\","
          << "\"path\":\""  << req.path   << "\","
          << "\"body\":"    << (req.body.empty() ? "\"\"" : req.body) << "}";
        return HttpResponse::make_json(200, o.str());
    });

    // ── Stress test endpoint ───────────────────
    router.add_route("GET", "/api/stress", [](const HttpRequest& req) {
        // Simulate variable latency to test thread pool behaviour
        auto it = req.query_params.find("delay_ms");
        if (it != req.query_params.end()) {
            int ms = std::min(std::stoi(it->second), 2000);  // cap at 2s
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        return HttpResponse::make_json(200,
            json_kv({{"status","ok"},
                     {"thread", std::to_string(
                         std::hash<std::thread::id>{}(
                             std::this_thread::get_id()) % 10000)}}));
    });

    // ── Middleware: request logger ─────────────
    router.use_middleware([](const HttpRequest& req, HttpResponse&) -> bool {
        // Return false to block; true to continue
        // Could add IP blacklist, content-type enforcement, etc.
        return true;
    });
}

// ─── Main ───────────────────────────────────────
int main(int argc, char* argv[]) {

    ServerConfig cfg;
    cfg.port         = 8080;
    cfg.threads      = std::thread::hardware_concurrency();
    cfg.backlog      = 512;
    cfg.recv_buf     = 16384;
    cfg.timeout_ms   = 5000;
    cfg.require_auth = false;   // set true to enforce Bearer tokens
    cfg.rate_limit   = 200;

    // Optional: override port via CLI
    if (argc > 1) cfg.port = std::stoi(argv[1]);
    if (argc > 2) cfg.threads = std::stoi(argv[2]);

    Router router;
    register_routes(router);

    Server server(cfg, router);
    g_server = &server;

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return 1;
    }

    return 0;
}
