CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -pthread
TARGET   = web_server
SRCS     = main.cpp
HEADERS  = thread_pool.h http.h router.h gateway.h server.h

.PHONY: all clean run test

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS)
	@echo "✅ Build successful: ./$(TARGET)"

run: $(TARGET)
	./$(TARGET) 8080 $(shell nproc)

# Quick functional test using curl
test: $(TARGET)
	@echo "--- Testing health endpoint ---"
	curl -s http://localhost:8080/health | python3 -m json.tool
	@echo "\n--- Testing root ---"
	curl -s http://localhost:8080/ | python3 -m json.tool
	@echo "\n--- Testing login ---"
	curl -s -X POST http://localhost:8080/login \
	  -H "Content-Type: application/json" \
	  -d '{"username":"alice","password":"secret"}' | python3 -m json.tool
	@echo "\n--- Testing users list ---"
	curl -s http://localhost:8080/api/users | python3 -m json.tool
	@echo "\n--- Testing echo ---"
	curl -s -X POST http://localhost:8080/api/echo \
	  -H "Content-Type: application/json" \
	  -d '{"hello":"world"}' | python3 -m json.tool
	@echo "\n--- Testing 404 ---"
	curl -s http://localhost:8080/not-found | python3 -m json.tool

# Load test with wrk (install: brew install wrk or apt install wrk)
load-test:
	@echo "Running 30s load test: 12 threads, 400 connections"
	wrk -t12 -c400 -d30s http://localhost:8080/health

clean:
	rm -f $(TARGET)
