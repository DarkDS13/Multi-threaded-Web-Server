#include "server.h"
#include "router.h"
#include "gateway.h"
#include "metrics.h"

#include <iostream>
#include <csignal>
#include <sstream>
#include <chrono>
#include <chrono>
#include <mutex>
#include <vector>

Server* g_server = nullptr;

struct GameEvent {
    int id;
    std::string payload; // Raw JSON string
};
std::mutex g_events_mutex;
std::vector<GameEvent> g_events;
int g_next_event_id = 0;
void signal_handler(int) {
    std::cout << "\n[INFO ] Shutting down gracefully...\n";
    if (g_server) { g_server->print_stats(); g_server->stop(); }
}

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

void register_routes(Router& router) {

    router.add_route("GET", "/", [](const HttpRequest&) {
        return HttpResponse::make_json(200,
            json_kv({{"service","Multithreaded Web Server"},{"status","running"},{"version","1.0.0"}}));
    });

    router.add_route("GET", "/health", [](const HttpRequest&) {
        return HttpResponse::make_json(200,
            json_kv({{"status","healthy"},{"uptime",std::to_string(get_metrics().uptime_seconds())+"s"}}));
    });

    // ── LIVE METRICS — polled by the dashboard every second ──
    router.add_route("GET", "/metrics", [](const HttpRequest&) {
        auto resp = HttpResponse::make_json(200, get_metrics().to_json());
        // Extra CORS headers so the HTML file (opened locally) can fetch
        resp.headers["Access-Control-Allow-Origin"]  = "*";
        resp.headers["Access-Control-Allow-Headers"] = "Content-Type";
        resp.headers["Cache-Control"]                = "no-store";
        return resp;
    });

    router.add_route("OPTIONS", "/metrics", [](const HttpRequest&) {
        HttpResponse r;
        r.status_code = 204; r.status_text = "No Content";
        r.headers["Access-Control-Allow-Origin"]  = "*";
        r.headers["Access-Control-Allow-Methods"] = "GET, OPTIONS";
        r.headers["Access-Control-Allow-Headers"] = "Content-Type";
        return r;
    });

    router.add_route("POST", "/login", [](const HttpRequest& req) {
        auto pos = req.body.find("\"username\":\"");
        std::string user = "anonymous";
        if (pos != std::string::npos) {
            auto s = pos+12, e = req.body.find("\"",s);
            if (e != std::string::npos) user = req.body.substr(s,e-s);
        }
        auto token = TokenValidator::generate(user);
        return HttpResponse::make_json(200,
            "{\"token\":\""+token+"\",\"expires_in\":3600,\"user\":\""+user+"\"}");
    });

    // ── MULTIPLAYER SYNCHRONIZATION ENDPOINTS ──
    router.add_route("GET", "/api/events", [](const HttpRequest& req) {
        int since_id = -1;
        auto pos = req.query_params.find("since");
        if (pos != req.query_params.end()) since_id = std::stoi(pos->second);
        
        std::lock_guard<std::mutex> lock(g_events_mutex);
        std::ostringstream o;
        o << "[";
        bool first = true;
        for (const auto& ev : g_events) {
            if (ev.id > since_id) {
                if (!first) o << ",";
                o << "{\"id\":" << ev.id << ",\"payload\":" << ev.payload << "}";
                first = false;
            }
        }
        o << "]";
        auto resp = HttpResponse::make_json(200, o.str());
        resp.headers["Access-Control-Allow-Origin"] = "*";
        return resp;
    });

    router.add_route("POST", "/api/events", [](const HttpRequest& req) {
        std::lock_guard<std::mutex> lock(g_events_mutex);
        GameEvent ev;
        ev.id = ++g_next_event_id;
        ev.payload = req.body.empty() ? "{}" : req.body;
        g_events.push_back(ev);
        
        if (g_events.size() > 500) {
            g_events.erase(g_events.begin(), g_events.begin() + 100);
        }
        
        auto resp = HttpResponse::make_json(201, "{\"status\":\"ok\"}");
        resp.headers["Access-Control-Allow-Origin"] = "*";
        return resp;
    });

    router.add_route("OPTIONS", "/api/events", [](const HttpRequest&) {
        HttpResponse r; r.status_code = 204; r.status_text = "No Content";
        r.headers["Access-Control-Allow-Origin"]  = "*";
        r.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS";
        r.headers["Access-Control-Allow-Headers"] = "Content-Type";
        return r;
    });

    router.add_route("GET", "/api/users", [](const HttpRequest&) {
        return HttpResponse::make_json(200,
            R"({"users":[{"id":1,"name":"Alice","role":"admin"},{"id":2,"name":"Bob","role":"viewer"}],"total":2})");
    });

    router.add_route("POST", "/api/users", [](const HttpRequest& req) {
        if (req.body.empty()) return HttpResponse::make_error(400, "Body required");
        return HttpResponse::make_json(201, "{\"message\":\"User created\",\"body\":"+req.body+"}","Created");
    });

    router.add_route("POST", "/api/echo", [](const HttpRequest& req) {
        std::ostringstream o;
        o << "{\"method\":\"" << req.method << "\",\"path\":\"" << req.path
          << "\",\"body\":" << (req.body.empty()?"\"\"":req.body) << "}";
        return HttpResponse::make_json(200, o.str());
    });

    router.add_route("GET", "/api/stress", [](const HttpRequest& req) {
        auto it = req.query_params.find("delay_ms");
        if (it != req.query_params.end()) {
            int ms = std::min(std::stoi(it->second), 2000);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        return HttpResponse::make_json(200,
            json_kv({{"status","ok"},{"thread",std::to_string(
                std::hash<std::thread::id>{}(std::this_thread::get_id())%10000)}}));
    });

    router.use_middleware([](const HttpRequest&, HttpResponse&) -> bool { return true; });
}

int main(int argc, char* argv[]) {
    ServerConfig cfg;
    cfg.port         = 8080;
    cfg.threads      = std::thread::hardware_concurrency();
    cfg.backlog      = 512;
    cfg.recv_buf     = 16384;
    cfg.timeout_ms   = 5000;
    cfg.require_auth = false;
    cfg.rate_limit   = 200;

    if (argc > 1) cfg.port    = std::stoi(argv[1]);
    if (argc > 2) cfg.threads = std::stoi(argv[2]);

    Router router;
    register_routes(router);

    Server server(cfg, router);
    g_server = &server;

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "[INFO ] Dashboard: open server_simulation.html in your browser\n";
    std::cout << "[INFO ] Metrics endpoint: http://localhost:" << cfg.port << "/metrics\n";

    try { server.start(); }
    catch (const std::exception& e) { std::cerr << "[FATAL] " << e.what() << "\n"; return 1; }
    return 0;
}
