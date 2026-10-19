#include "cardinal/executor.h"

namespace cardinal {

ThreadPoolExecutor::ThreadPoolExecutor(int threads) {
    for (int i = 0; i < threads; ++i) threads_.emplace_back([this] { work(); });
}

ThreadPoolExecutor::~ThreadPoolExecutor() {
    {
        std::lock_guard lk(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    for (auto& t : threads_) t.join();
}

void ThreadPoolExecutor::post(std::function<void()> job) {
    {
        std::lock_guard lk(mutex_);
        if (stop_) return;
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void ThreadPoolExecutor::work() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock lk(mutex_);
            wake_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
}

}  // namespace cardinal
