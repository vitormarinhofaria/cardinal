#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace cardinal {

// Where background work runs. Never the UI thread.
class Executor {
public:
    virtual ~Executor() = default;
    virtual void post(std::function<void()> job) = 0;
};

// Fixed pool. The destructor finishes the job each worker is running and drops queued ones.
class ThreadPoolExecutor : public Executor {
public:
    explicit ThreadPoolExecutor(int threads = 4);
    ~ThreadPoolExecutor() override;
    ThreadPoolExecutor(const ThreadPoolExecutor&) = delete;
    ThreadPoolExecutor& operator=(const ThreadPoolExecutor&) = delete;

    void post(std::function<void()> job) override;

private:
    void work();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace cardinal
