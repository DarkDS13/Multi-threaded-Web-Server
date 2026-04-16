#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <stdexcept>

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads) : stop_(false), active_tasks_(0) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex_);
                        condition_.wait(lock, [this] {
                            return stop_ || !tasks_.empty();
                        });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                        ++active_tasks_;
                    }
                    task();
                    {
                        std::lock_guard<std::mutex> lock(queue_mutex_);
                        --active_tasks_;
                    }
                    done_condition_.notify_all();
                }
            });
        }
    }

    template<typename F>
    void enqueue(F&& task) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (stop_) throw std::runtime_error("ThreadPool is stopped");
            tasks_.emplace(std::forward<F>(task));
        }
        condition_.notify_one();
    }

    void wait_all() {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        done_condition_.wait(lock, [this] {
            return tasks_.empty() && active_tasks_ == 0;
        });
    }

    size_t queue_size() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queue_mutex_));
        return tasks_.size();
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for (auto& w : workers_) w.join();
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::condition_variable done_condition_;
    std::atomic<bool> stop_;
    std::atomic<int> active_tasks_;
};
