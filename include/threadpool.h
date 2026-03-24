#pragma once
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// Fixed-size thread pool. Work items are file paths; each item is processed
// by calling the Task function supplied at construction time.
//
// Task signature: void(const fs::path& current, const fs::path* next)
//   `next` is a hint to the next path in the queue (for prefetch), or nullptr.
//
// Usage:
//   ThreadPool pool(n, [&](const fs::path& f, const fs::path* next) { ... });
//   for (auto& f : files) pool.enqueue(f);
//   pool.wait();
class ThreadPool {
public:
    using Task = std::function<void(const fs::path&, const fs::path*)>;

    explicit ThreadPool(size_t n_threads, Task task);
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void enqueue(fs::path path);
    void wait();

private:
    void worker_loop();

    Task                     task_;
    std::vector<std::thread> workers_;
    std::queue<fs::path>     queue_;
    std::mutex               mtx_;
    std::condition_variable  cv_work_;
    std::condition_variable  cv_done_;
    size_t                   pending_{0};
    bool                     shutdown_{false};
};
