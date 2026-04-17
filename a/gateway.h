#pragma once
#include "http.h"
#include "router.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <sstream>
#include <functional>
#include <atomic>

// ─────────────────────────────────────────────
// Simple in-memory Rate Limiter (per IP)
// In production this would live in Redis/Memcached
// to maintain true statelessness across nodes.
// ─────────────────────────────────────────────
class RateLimiter {
    struct Bucket { int count; std::chrono::steady_clock::time_point reset_at; };
    std::unordered_map<std::string, Bucket> buckets_;
    std::mutex mu_;
    int limit_;
    std::chrono::seconds window_;
public:
    RateLimiter(int limit = 100,
                std::chrono::seconds window = std::chrono::seconds(60))
        : limit_(limit), window_(window) {}

    bool allow(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mu_);
        auto now = std::chrono::steady_clock::now();
        auto& b = buckets_[ip];
        if (b.count == 0 || now > b.reset_at) {
            b.count = 1;
            b.reset_at = now + window_;
            return true;
        }
        if (b.count >= limit_) return false;
        ++b.count;
        return true;
    }
};

// ─────────────────────────────────────────────
// Stateless JWT-like token validator
// Tokens are self-describing (no server state needed).
// Format:  base64(header).base64(payload).signature
// ─────────────────────────────────────────────
class TokenValidator {
public:
    // In a real system, verify HMAC-SHA256 signature here.
    // For this implementation we validate structure & expiry field.
    static bool validate(const std::string& token, std::string& subject) {
        auto parts = split(token, '.');
        if (parts.size() != 3) return false;
        // Decode payload (base64 -> JSON-like string)
        // For demo: token format is "user_id.expiry.sig"
        subject = parts[0];
        try {
            long expiry = std::stol(parts[1]);
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return expiry > now;
        } catch (...) { return false; }
    }

    static std::string generate(const std::string& user_id, int ttl_seconds = 3600) {
        auto expiry = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() + ttl_seconds;
        // Signature placeholder — replace with HMAC-SHA256 in production
        std::string sig = std::to_string(std::hash<std::string>{}(
            user_id + std::to_string(expiry) + "SECRET_KEY"));
        return user_id + "." + std::to_string(expiry) + "." + sig;
    }

private:
    static std::vector<std::string> split(const std::string& s, char d) {
        std::vector<std::string> res;
        std::istringstream ss(s);
        std::string t;
        while (std::getline(ss, t, d)) res.push_back(t);
        return res;
    }
};

// ─────────────────────────────────────────────
// API Gateway  –  sits in front of the Router.
// Enforces: rate limiting, auth, CORS, request-id injection.
// ─────────────────────────────────────────────
class ApiGateway {
public:
    ApiGateway(Router& router,
               bool require_auth = false,
               int rate_limit = 2000)
        : router_(router),
          require_auth_(require_auth),
          rate_limiter_(rate_limit),
          request_counter_(0) {}

    HttpResponse process(const HttpRequest& req, const std::string& client_ip) {
        auto req_id = next_request_id();

        // 1. Rate limiting
        // 1. Rate limiting (Whitelist the dashboard metrics and multiplayer heartbeat)
        if (req.path != "/metrics" && req.path != "/api/events" && req.method != "OPTIONS" && !rate_limiter_.allow(client_ip)) {
            auto r = HttpResponse::make_error(429, "Too Many Requests");
            inject_common_headers(r, req_id);
            return r;
        }

        // 2. CORS preflight
        if (req.method == "OPTIONS") {
            HttpResponse r;
            r.status_code = 204;
            r.status_text = "No Content";
            inject_cors_headers(r);
            inject_common_headers(r, req_id);
            return r;
        }

        // 3. Auth check (Bearer token)
        std::string subject;
        if (require_auth_ && !is_public_path(req.path)) {
            auto it = req.headers.find("authorization");
            if (it == req.headers.end() ||
                it->second.rfind("Bearer ", 0) != 0 ||
                !TokenValidator::validate(it->second.substr(7), subject)) {
                auto r = HttpResponse::make_error(401, "Unauthorized");
                inject_common_headers(r, req_id);
                return r;
            }
        }

        // 4. Route to handler
        auto resp = router_.handle(req);

        // 5. Inject standard gateway headers
        inject_common_headers(resp, req_id);
        inject_cors_headers(resp);
        return resp;
    }

    // Token generation endpoint helper
    HttpResponse login(const std::string& user_id) {
        auto token = TokenValidator::generate(user_id);
        return HttpResponse::make_json(200,
            "{\"token\":\"" + token + "\",\"expires_in\":3600}");
    }

private:
    Router& router_;
    bool require_auth_;
    RateLimiter rate_limiter_;
    std::atomic<uint64_t> request_counter_;

    std::string next_request_id() {
        return "req-" + std::to_string(++request_counter_);
    }

    bool is_public_path(const std::string& path) {
        return path == "/health" || path == "/login" || path == "/";
    }

    void inject_common_headers(HttpResponse& r, const std::string& req_id) {
        r.headers["X-Request-ID"] = req_id;
        r.headers["X-Server"] = "CustomCppGateway/1.0";
        r.headers["Cache-Control"] = "no-store";
    }

    void inject_cors_headers(HttpResponse& r) {
        r.headers["Access-Control-Allow-Origin"] = "*";
        r.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS";
        r.headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization";
    }
};
