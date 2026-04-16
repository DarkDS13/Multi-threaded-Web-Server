#pragma once
#include <string>
#include <unordered_map>
#include <sstream>
#include <algorithm>

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string query_string;
    std::unordered_map<std::string, std::string> query_params;
};

struct HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
        for (auto& [k, v] : headers)
            oss << k << ": " << v << "\r\n";
        oss << "Content-Length: " << body.size() << "\r\n";
        oss << "Connection: close\r\n\r\n";
        oss << body;
        return oss.str();
    }

    static HttpResponse make_json(int code, const std::string& json_body,
                                   const std::string& status = "OK") {
        HttpResponse r;
        r.status_code = code;
        r.status_text = status;
        r.headers["Content-Type"] = "application/json";
        r.headers["X-Powered-By"] = "CustomCppServer/1.0";
        r.body = json_body;
        return r;
    }

    static HttpResponse make_error(int code, const std::string& msg) {
        std::string status;
        switch (code) {
            case 400: status = "Bad Request"; break;
            case 404: status = "Not Found"; break;
            case 405: status = "Method Not Allowed"; break;
            case 500: status = "Internal Server Error"; break;
            default:  status = "Error";
        }
        return make_json(code,
            "{\"error\":\"" + msg + "\",\"code\":" + std::to_string(code) + "}",
            status);
    }
};

class HttpParser {
public:
    static bool parse(const std::string& raw, HttpRequest& req) {
        std::istringstream stream(raw);
        std::string request_line;
        if (!std::getline(stream, request_line)) return false;
        if (!request_line.empty() && request_line.back() == '\r')
            request_line.pop_back();

        std::istringstream rl(request_line);
        rl >> req.method >> req.path >> req.version;
        if (req.method.empty() || req.path.empty()) return false;

        // Parse query string
        auto qpos = req.path.find('?');
        if (qpos != std::string::npos) {
            req.query_string = req.path.substr(qpos + 1);
            req.path = req.path.substr(0, qpos);
            parse_query(req.query_string, req.query_params);
        }

        // Parse headers
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string val = line.substr(colon + 1);
                // Trim
                val.erase(0, val.find_first_not_of(" \t"));
                val.erase(val.find_last_not_of(" \t") + 1);
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                req.headers[key] = val;
            }
        }

        // Parse body
        std::ostringstream body_stream;
        body_stream << stream.rdbuf();
        req.body = body_stream.str();
        return true;
    }

private:
    static void parse_query(const std::string& qs,
                             std::unordered_map<std::string, std::string>& out) {
        std::istringstream ss(qs);
        std::string token;
        while (std::getline(ss, token, '&')) {
            auto eq = token.find('=');
            if (eq != std::string::npos)
                out[token.substr(0, eq)] = token.substr(eq + 1);
            else
                out[token] = "";
        }
    }
};
