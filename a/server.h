#pragma once
#include "thread_pool.h"
#include "http.h"
#include "gateway.h"
#include "metrics.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

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
#include <thread>
#include <unordered_map>

class Logger {
    std::mutex mu_;
public:
    void log(const std::string& level, const std::string& msg) {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S") << "] [" << level << "] " << msg << "\n" << std::flush;
    }
    void info (const std::string& m) { log("INFO ", m); }
    void warn (const std::string& m) { log("WARN ", m); }
    void error(const std::string& m) { log("ERROR", m); }
};

struct ServerConfig {
    int  port         = 8080;
    int  threads      = (int)std::thread::hardware_concurrency();
    int  backlog      = 1024;
    int  recv_buf     = 8192;
    int  timeout_ms   = 5000;
    bool require_auth = false;
    int  rate_limit   = 200;
};

class Server {
public:
    Server(ServerConfig cfg, Router& router)
        : cfg_(cfg),
          gateway_(router, cfg.require_auth, cfg.rate_limit),
          pool_(cfg.threads),
          running_(false)
    {
        auto& m = get_metrics();
        m.num_threads = cfg.threads;
        std::lock_guard<std::mutex> lk(m.thread_mu);
        m.thread_states.resize(cfg.threads);
        for (int i = 0; i < cfg.threads; ++i)
            m.thread_states[i] = {i, "idle", "", 0};
    }

    void start() {
        setup_socket();
        running_ = true;
        logger_.info("Server listening on port " + std::to_string(cfg_.port) + " | workers: " + std::to_string(cfg_.threads));
        while (running_) {
            sockaddr_in client_addr{};
            socklen_t   client_len = sizeof(client_addr);
            int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) { if (running_) logger_.warn("accept() failed"); continue; }
            std::string ip = inet_ntoa(client_addr.sin_addr);
            get_metrics().total_requests++;
            get_metrics().active_conns++;
            get_metrics().record_request();
            get_metrics().queued_tasks = pool_.queue_size();
            pool_.enqueue([this, client_fd, ip]() {
                get_metrics().queued_tasks = pool_.queue_size();
                handle_connection(client_fd, ip);
                get_metrics().active_conns--;
            });
        }
        close(server_fd_);
    }

    void stop() {
        running_ = false;
        int wake = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(cfg_.port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        connect(wake, (sockaddr*)&addr, sizeof(addr));
        close(wake);
    }

    void print_stats() const {
        auto& m = get_metrics();
        logger_.info("=== Final: requests=" + std::to_string(m.total_requests.load()) +
                     " 200=" + std::to_string(m.stat_200.load()) +
                     " 4xx=" + std::to_string(m.stat_4xx.load()) + " ===");
    }

private:
    ServerConfig      cfg_;
    ApiGateway        gateway_;
    ThreadPool        pool_;
    int               server_fd_ = -1;
    std::atomic<bool> running_;
    mutable Logger    logger_;

    int thread_index() {
        static std::mutex mu;
        static std::unordered_map<std::thread::id, int> map;
        static std::atomic<int> counter{0};
        std::lock_guard<std::mutex> lk(mu);
        auto id = std::this_thread::get_id();
        auto it = map.find(id);
        if (it != map.end()) return it->second;
        int idx = counter++ % cfg_.threads;
        map[id] = idx;
        return idx;
    }

    void set_thread_state(int idx, const std::string& state,
        const std::string& path = "", int progress = 0) {
        auto& m = get_metrics();
        std::lock_guard<std::mutex> lk(m.thread_mu);
        if (idx >= 0 && idx < (int)m.thread_states.size()) {
            m.thread_states[idx] = {idx, state, path, progress};
        }
    }

    void setup_socket() {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) throw std::runtime_error("socket() failed");
        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
        setsockopt(server_fd_, SOL_SOCKET, SO_RCVBUF, &cfg_.recv_buf, sizeof(cfg_.recv_buf));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(cfg_.port);
        if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0)
            throw std::runtime_error("bind() failed on port " + std::to_string(cfg_.port));
        if (listen(server_fd_, cfg_.backlog) < 0)
            throw std::runtime_error("listen() failed");
    }

    void handle_connection(int fd, const std::string& ip) {
        int tidx = thread_index();
        set_socket_timeout(fd, cfg_.timeout_ms);
#ifdef SO_NOSIGPIPE
        int nosig = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
        auto t_start = std::chrono::steady_clock::now();
        set_thread_state(tidx, "busy", "reading", 10);

        std::string raw = read_request(fd);
        if (raw.empty()) { close(fd); set_thread_state(tidx,"idle"); return; }

        set_thread_state(tidx, "busy", "parsing", 30);
        HttpRequest req;
        if (!HttpParser::parse(raw, req)) {
            get_metrics().stat_500++;
            send_response(fd, HttpResponse::make_error(400, "Malformed request"));
            close(fd); set_thread_state(tidx,"idle"); return;
        }

        set_thread_state(tidx, "busy", req.path, 60);
        auto resp = gateway_.process(req, ip);
        set_thread_state(tidx, "busy", req.path, 90);

        auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start).count();
        get_metrics().record_latency(latency_ms);

        auto& m = get_metrics();
        if      (resp.status_code == 200 || resp.status_code == 201) m.stat_200++;
        else if (resp.status_code == 401) { m.stat_401++; m.stat_4xx++; }
        else if (resp.status_code == 429)   m.stat_429++;
        else if (resp.status_code >= 400)   m.stat_4xx++;
        else if (resp.status_code >= 500)   m.stat_500++;

        std::string status_str =
            (resp.status_code == 429) ? "rl"   :
            (resp.status_code == 401) ? "auth" :
            (resp.status_code >= 400) ? "err"  : "ok";
        m.record_ip(ip, req.path, status_str);

        logger_.info(ip + " " + req.method + " " + req.path +
                     " -> " + std::to_string(resp.status_code) +
                     " [" + std::to_string(latency_ms) + "ms]");

        send_response(fd, resp);
        close(fd);
        set_thread_state(tidx, "idle", "", 0);
    }

    std::string read_request(int fd) {
        std::string buf;
        buf.reserve(cfg_.recv_buf);
        char chunk[4096];
        while (true) {
            ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            buf.append(chunk, n);
            if (buf.find("\r\n\r\n") != std::string::npos) break;
        }
        return buf;
    }

    void send_response(int fd, const HttpResponse& resp) {
        std::string raw = resp.to_string();
        size_t sent = 0;
        while (sent < raw.size()) {
            ssize_t n = send(fd, raw.c_str()+sent, raw.size()-sent, MSG_NOSIGNAL);
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
