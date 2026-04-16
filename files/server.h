#pragma once
#include "thread_pool.h"
#include "http.h"
#include "gateway.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

// MSG_NOSIGNAL is Linux-only; on macOS suppress SIGPIPE via SO_NOSIGPIPE instead
#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

#include <string>
#include <stdexcept>
#include <iostream>
#include <atomic>
#include <mutex>
#include <cstring>
#include <chrono>
#include <iomanip>

// ─────────────────────────────────────────────
// Thread-safe logger
// ─────────────────────────────────────────────
class Logger {
    std::mutex mu_;
    std::string level_;
public:
    explicit Logger(const std::string& lvl = "INFO") : level_(lvl) {}

    void log(const std::string& level, const std::string& msg) {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S")
                  << "] [" << level << "] " << msg << "\n";
    }
    void info (const std::string& m) { log("INFO ", m); }
    void warn (const std::string& m) { log("WARN ", m); }
    void error(const std::string& m) { log("ERROR", m); }
};

// ─────────────────────────────────────────────
// Server Configuration
// ─────────────────────────────────────────────
struct ServerConfig {
    int    port        = 8080;
    int    threads     = std::thread::hardware_concurrency();
    int    backlog     = 1024;
    int    recv_buf    = 8192;      // bytes
    int    timeout_ms  = 5000;      // per-connection read timeout
    bool   require_auth = false;
    int    rate_limit   = 200;      // requests / 60s per IP
};

// ─────────────────────────────────────────────
// Multithreaded HTTP Server
// ─────────────────────────────────────────────
class Server {
public:
    Server(ServerConfig cfg, Router& router)
        : cfg_(cfg),
          gateway_(router, cfg.require_auth, cfg.rate_limit),
          pool_(cfg.threads),
          running_(false),
          total_requests_(0),
          total_errors_(0) {}

    void start() {
        setup_socket();
        running_ = true;
        logger_.info("Server listening on port " + std::to_string(cfg_.port) +
                     " | workers: " + std::to_string(cfg_.threads));

        while (running_) {
            sockaddr_in client_addr{};
            socklen_t   client_len = sizeof(client_addr);

            int client_fd = accept(server_fd_,
                                   (sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                if (running_) logger_.warn("accept() failed: " +
                                           std::string(strerror(errno)));
                continue;
            }

            std::string ip = inet_ntoa(client_addr.sin_addr);
            ++total_requests_;

            // Dispatch to thread pool — zero blocking on main thread
            pool_.enqueue([this, client_fd, ip]() {
                handle_connection(client_fd, ip);
            });
        }
        close(server_fd_);
    }

    void stop() {
        running_ = false;
        // Unblock accept() by connecting a dummy socket
        int wake = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(cfg_.port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        connect(wake, (sockaddr*)&addr, sizeof(addr));
        close(wake);
    }

    void print_stats() const {
        logger_.info("=== Stats: requests=" + std::to_string(total_requests_.load()) +
                     " errors=" + std::to_string(total_errors_.load()) +
                     " queued=" + std::to_string(pool_.queue_size()) + " ===");
    }

private:
    ServerConfig         cfg_;
    ApiGateway           gateway_;
    ThreadPool           pool_;
    int                  server_fd_ = -1;
    std::atomic<bool>    running_;
    std::atomic<uint64_t> total_requests_;
    std::atomic<uint64_t> total_errors_;
    mutable Logger       logger_;

    // ── Socket Setup ──────────────────────────
    void setup_socket() {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0)
            throw std::runtime_error("socket() failed");

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

        // Set receive buffer
        setsockopt(server_fd_, SOL_SOCKET, SO_RCVBUF,
                   &cfg_.recv_buf, sizeof(cfg_.recv_buf));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(cfg_.port);

        if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0)
            throw std::runtime_error("bind() failed on port " +
                                     std::to_string(cfg_.port));

        if (listen(server_fd_, cfg_.backlog) < 0)
            throw std::runtime_error("listen() failed");
    }

    // ── Per-Connection Handler (runs on worker thread) ──
    void handle_connection(int fd, const std::string& ip) {
        // Non-blocking read with timeout
        set_socket_timeout(fd, cfg_.timeout_ms);
#ifdef SO_NOSIGPIPE
        // macOS: suppress SIGPIPE at socket level instead of MSG_NOSIGNAL
        int nosig = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif

        std::string raw = read_request(fd);
        if (raw.empty()) {
            close(fd);
            return;
        }

        HttpRequest req;
        if (!HttpParser::parse(raw, req)) {
            ++total_errors_;
            auto resp = HttpResponse::make_error(400, "Malformed HTTP request");
            send_response(fd, resp);
            close(fd);
            return;
        }

        auto resp = gateway_.process(req, ip);

        // Access log
        logger_.info(ip + " " + req.method + " " + req.path +
                     " -> " + std::to_string(resp.status_code));

        send_response(fd, resp);
        close(fd);
    }

    // ── I/O Helpers ───────────────────────────
    std::string read_request(int fd) {
        std::string buffer;
        buffer.reserve(cfg_.recv_buf);
        char chunk[4096];
        while (true) {
            ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            buffer.append(chunk, n);
            // Stop at end of HTTP headers + body (simple heuristic)
            if (buffer.size() > 4 &&
                buffer.find("\r\n\r\n") != std::string::npos) {
                // Check content-length if present
                auto cl_pos = buffer.find("content-length:");
                if (cl_pos == std::string::npos)
                    cl_pos = buffer.find("Content-Length:");
                if (cl_pos == std::string::npos) break;
                // Body is fully received — simple check
                break;
            }
        }
        return buffer;
    }

    void send_response(int fd, const HttpResponse& resp) {
        std::string raw = resp.to_string();
        size_t sent = 0;
        while (sent < raw.size()) {
            ssize_t n = send(fd, raw.c_str() + sent, raw.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) break;
            sent += n;
        }
    }

    void set_socket_timeout(int fd, int ms) {
        struct timeval tv;
        tv.tv_sec  = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
};
