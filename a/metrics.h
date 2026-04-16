#pragma once
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <sstream>
#include <chrono>
#include <unordered_map>
#include <numeric>

// ─────────────────────────────────────────────────────────
//  Global live metrics — written by worker threads,
//  read by the /metrics endpoint (polled by the dashboard).
// ─────────────────────────────────────────────────────────
struct Metrics {
    // ── Counters (lock-free) ──────────────────────────────
    std::atomic<uint64_t> total_requests {0};
    std::atomic<uint64_t> stat_200       {0};
    std::atomic<uint64_t> stat_4xx       {0};
    std::atomic<uint64_t> stat_429       {0};
    std::atomic<uint64_t> stat_401       {0};
    std::atomic<uint64_t> stat_500       {0};
    std::atomic<uint64_t> active_conns   {0};  // currently in-flight
    std::atomic<uint64_t> queued_tasks   {0};  // waiting in thread pool

    // ── Thread state snapshot (written under mutex) ───────
    struct ThreadSnap {
        int    id;
        std::string state;   // "idle" | "busy" | "waiting"
        std::string path;    // current request path (if busy)
        int    progress;     // 0-100 fake progress for UI
    };
    std::vector<ThreadSnap> thread_states;
    std::mutex thread_mu;

    // ── Latency ring buffer ───────────────────────────────
    std::vector<uint64_t> latency_samples;  // ms
    std::mutex latency_mu;
    static constexpr size_t LATENCY_BUF = 50;

    void record_latency(uint64_t ms) {
        std::lock_guard<std::mutex> lk(latency_mu);
        latency_samples.push_back(ms);
        if (latency_samples.size() > LATENCY_BUF)
            latency_samples.erase(latency_samples.begin());
    }

    uint64_t avg_latency_ms() {
        std::lock_guard<std::mutex> lk(latency_mu);
        if (latency_samples.empty()) return 0;
        return std::accumulate(latency_samples.begin(),
                               latency_samples.end(), 0ULL)
               / latency_samples.size();
    }

    // ── Per-IP request counts ─────────────────────────────
    std::unordered_map<std::string, uint64_t> ip_counts;
    std::unordered_map<std::string, std::string> ip_last_path;
    std::unordered_map<std::string, std::string> ip_last_status;
    std::mutex ip_mu;

    void record_ip(const std::string& ip, const std::string& path,
                   const std::string& status) {
        std::lock_guard<std::mutex> lk(ip_mu);
        ip_counts[ip]++;
        ip_last_path[ip]   = path;
        ip_last_status[ip] = status;
    }

    // ── RPS window ────────────────────────────────────────
    std::vector<std::chrono::steady_clock::time_point> rps_window;
    std::mutex rps_mu;

    void record_request() {
        std::lock_guard<std::mutex> lk(rps_mu);
        auto now = std::chrono::steady_clock::now();
        rps_window.push_back(now);
        // Prune older than 1s
        auto cutoff = now - std::chrono::seconds(1);
        while (!rps_window.empty() && rps_window.front() < cutoff)
            rps_window.erase(rps_window.begin());
    }

    uint64_t current_rps() {
        std::lock_guard<std::mutex> lk(rps_mu);
        auto now = std::chrono::steady_clock::now();
        auto cutoff = now - std::chrono::seconds(1);
        uint64_t count = 0;
        for (auto& t : rps_window) if (t >= cutoff) count++;
        return count;
    }

    // ── Server start time ─────────────────────────────────
    std::chrono::steady_clock::time_point start_time =
        std::chrono::steady_clock::now();

    uint64_t uptime_seconds() const {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();
    }

    int num_threads = 0;

    // ── Serialize to JSON ─────────────────────────────────
    std::string to_json() {
        std::ostringstream o;
        o << "{";

        // Counters
        o << "\"total_requests\":"  << total_requests.load() << ",";
        o << "\"stat_200\":"        << stat_200.load()       << ",";
        o << "\"stat_4xx\":"        << stat_4xx.load()       << ",";
        o << "\"stat_429\":"        << stat_429.load()       << ",";
        o << "\"stat_401\":"        << stat_401.load()       << ",";
        o << "\"stat_500\":"        << stat_500.load()       << ",";
        o << "\"active_conns\":"    << active_conns.load()   << ",";
        o << "\"queued_tasks\":"    << queued_tasks.load()   << ",";
        o << "\"rps\":"             << current_rps()         << ",";
        o << "\"avg_latency_ms\":"  << avg_latency_ms()      << ",";
        o << "\"uptime_seconds\":"  << uptime_seconds()      << ",";
        o << "\"num_threads\":"     << num_threads           << ",";

        // Thread states
        {
            std::lock_guard<std::mutex> lk(thread_mu);
            o << "\"threads\":[";
            bool first = true;
            for (auto& t : thread_states) {
                if (!first) o << ",";
                o << "{\"id\":" << t.id
                  << ",\"state\":\"" << t.state << "\""
                  << ",\"path\":\"" << t.path << "\""
                  << ",\"progress\":" << t.progress << "}";
                first = false;
            }
            o << "],";
        }

        // Top IPs
        {
            std::lock_guard<std::mutex> lk(ip_mu);
            o << "\"clients\":[";
            bool first = true;
            int n = 0;
            for (auto& [ip, cnt] : ip_counts) {
                if (n++ >= 8) break;
                if (!first) o << ",";
                std::string status = ip_last_status.count(ip) ? ip_last_status[ip] : "ok";
                std::string path   = ip_last_path.count(ip)   ? ip_last_path[ip]   : "/";
                o << "{\"ip\":\"" << ip << "\""
                  << ",\"count\":" << cnt
                  << ",\"path\":\"" << path << "\""
                  << ",\"status\":\"" << status << "\"}";
                first = false;
            }
            o << "]";
        }

        o << "}";
        return o.str();
    }
};

// ── Global singleton ──────────────────────────────────────
inline Metrics& get_metrics() {
    static Metrics m;
    return m;
}
