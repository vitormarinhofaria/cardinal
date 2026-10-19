#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cardinal/http.h"

namespace cardinal {

enum class Priority { Interactive = 0, Visible = 1, Background = 2 };

class Clock {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    virtual ~Clock() = default;
    virtual TimePoint now() = 0;
};

class SystemClock : public Clock {
public:
    TimePoint now() override { return std::chrono::steady_clock::now(); }
};

// Trello allows 100 req/10 s per token and 300 req/10 s per API key (shared by every user of the
// key); the default keeps headroom below the token limit.
struct RateLimit {
    int maxRequests = 90;
    std::chrono::milliseconds window{10000};
    std::chrono::milliseconds retryBase{1000};  // backoff after a 429 doubles from here
    int maxRetries = 4;                         // after this many retries a 429 is returned as is
};

// Sends requests in priority order without exceeding the rate limit, retrying 429s with backoff.
// 401 and other statuses are returned to the caller untouched.
class Scheduler {
public:
    // autoStart=false is for tests: the caller drives time by calling pump().
    Scheduler(HttpClient& http, RateLimit limit, Clock& clock, bool autoStart = true);
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    std::future<HttpResponse> submit(Priority priority, Method method, std::string url,
                                     std::string body = "");

    // Dispatches whatever the limit and the clock allow now. Returns when it wants to be called
    // again, or nullopt if idle until the next submit.
    std::optional<Clock::TimePoint> pump();

private:
    struct Pending {
        Method method;
        std::string url;
        std::string body;
        std::shared_ptr<std::promise<HttpResponse>> promise;
        Priority priority;
        int retries = 0;
        Clock::TimePoint notBefore{};
    };
    struct InFlight {
        Pending pending;
        std::future<HttpResponse> response;
    };

    std::optional<Clock::TimePoint> pumpLocked();
    bool collectLocked();
    bool dispatchLocked();
    void run();

    HttpClient& http_;
    RateLimit limit_;
    Clock& clock_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Pending> queues_[3];
    std::list<InFlight> inFlight_;
    std::deque<Clock::TimePoint> sent_;  // dispatch times inside the trailing window
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace cardinal
