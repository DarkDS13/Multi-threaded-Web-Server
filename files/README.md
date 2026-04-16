# Multithreaded Web Server with Stateless API Gateway
### C++17 · POSIX Sockets · Thread Pool · JWT-style Auth · Rate Limiting

---

## Architecture Overview

```
Client Request
      │
      ▼
┌─────────────┐       accept() loop (main thread)
│   Server    │──────►  dispatches fd to ThreadPool
└─────────────┘
      │
      ▼  (worker thread)
┌─────────────┐
│ API Gateway │  ① Rate Limiter (per-IP token bucket)
│             │  ② CORS preflight handler
│             │  ③ Bearer token auth (stateless JWT-style)
│             │  ④ Request-ID injection
└─────────────┘
      │
      ▼
┌─────────────┐
│   Router    │  Method + path matching → Handler function
└─────────────┘
      │
      ▼
┌─────────────┐
│  Response   │  Serialized to HTTP/1.1 wire format → sent back
└─────────────┘
```

---

## File Structure

| File            | Responsibility                                      |
|-----------------|-----------------------------------------------------|
| `thread_pool.h` | Dynamic worker pool, task queue, mutex+CV sync      |
| `http.h`        | HttpRequest/HttpResponse structs, HTTP/1.1 parser   |
| `router.h`      | Route table, middleware chain                       |
| `gateway.h`     | Rate limiter, token validator, CORS, auth layer     |
| `server.h`      | Socket setup, accept loop, per-connection I/O       |
| `main.cpp`      | Config, route registration, signal handling         |

---

## Build & Run

```bash
# Build
make

# Run on port 8080 with auto-detected CPU cores
./web_server

# Custom port and thread count
./web_server 9090 8

# Run functional tests (server must be running)
make test

# Load test (requires wrk)
make load-test
```

---

## API Endpoints

| Method | Path          | Auth | Description               |
|--------|---------------|------|---------------------------|
| GET    | `/`           | No   | Service info              |
| GET    | `/health`     | No   | Health check              |
| POST   | `/login`      | No   | Get Bearer token          |
| GET    | `/api/users`  | *    | List users                |
| POST   | `/api/users`  | *    | Create user               |
| POST   | `/api/echo`   | *    | Echo request back         |
| GET    | `/api/stress` | *    | Latency sim (`?delay_ms=`) |

\* Auth required only when `cfg.require_auth = true`

---

## Key Design Decisions

### Thread Pool (not Thread-per-Request)
Creating a new OS thread per connection fails at ~10k connections (C10k problem).
The pool keeps **N = nproc** threads alive permanently, feeding them work via a
mutex-protected task queue. This bounds memory usage regardless of traffic.

### Stateless Gateway
No session data lives on the server. Auth state is encoded in a self-describing
**signed token** (`user_id.expiry.signature`) that each request carries.
This means any server instance can validate any request — enabling horizontal
scaling with a simple load balancer and zero sticky sessions.

### Non-blocking I/O
Each worker socket has `SO_RCVTIMEO` / `SO_SNDTIMEO` set. A slow client
never monopolises a thread indefinitely; it times out and the thread returns
to the pool.

### Rate Limiter
Per-IP sliding-window counter (100 req/60s default). In a multi-node deployment
move the counter to Redis to share state across instances without breaking
the stateless principle.

---

## Enabling Auth

```cpp
// main.cpp
cfg.require_auth = true;
```

Then authenticate:
```bash
# Get token
TOKEN=$(curl -s -X POST http://localhost:8080/login \
  -d '{"username":"alice"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])")

# Call protected endpoint
curl -H "Authorization: Bearer $TOKEN" http://localhost:8080/api/users
```

---

## Production Hardening Checklist

- [ ] Replace placeholder HMAC signature with OpenSSL HMAC-SHA256
- [ ] Move RateLimiter state to Redis (true multi-node statelessness)  
- [ ] Add TLS via OpenSSL or wrap with nginx/Caddy as TLS terminator
- [ ] Upgrade HTTP parser to handle chunked transfer encoding
- [ ] Add `/api/metrics` endpoint (request count, latency histograms)
- [ ] Use `epoll` edge-triggered mode for higher connection density
