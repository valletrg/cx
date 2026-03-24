#include "threadpool.h"

ThreadPool::ThreadPool(size_t n_threads, Task task)
    : task_(std::move(task)) {
    workers_.reserve(n_threads);
    for (size_t i = 0; i < n_threads; ++i)
        workers_.emplace_back([this] { worker_loop(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        shutdown_ = true;
    }
    cv_work_.notify_all();
    for (auto& w : workers_)
        if (w.joinable()) w.join();
}

void ThreadPool::enqueue(fs::path path) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        ++pending_;
        queue_.push(std::move(path));
    }
    cv_work_.notify_one();
}

void ThreadPool::wait() {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_done_.wait(lock, [this] { return pending_ == 0; });
}

void ThreadPool::worker_loop() {
    while (true) {
        fs::path path;
        fs::path next;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_work_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
            if (queue_.empty()) return;
            path = std::move(queue_.front());
            queue_.pop();
            // Peek at next item for prefetch hint (Opt 5)
            if (!queue_.empty())
                next = queue_.front();
        }
        const fs::path* next_ptr = next.empty() ? nullptr : &next;
        task_(path, next_ptr);
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (--pending_ == 0)
                cv_done_.notify_all();
        }
    }
}
