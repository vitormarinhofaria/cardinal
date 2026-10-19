#include "cardinal/scheduler.h"

#include <algorithm>

namespace cardinal {
namespace {

using namespace std::chrono_literals;
constexpr auto kPollInterval = 5ms;  // how often in-flight requests are checked for completion

}  // namespace

Scheduler::Scheduler(HttpClient& http, RateLimit limit, Clock& clock, bool autoStart)
    : http_(http), limit_(limit), clock_(clock) {
    if (autoStart) worker_ = std::thread([this] { run(); });
}

Scheduler::~Scheduler() {
    {
        std::lock_guard lk(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::future<HttpResponse> Scheduler::submit(Priority priority, Method method, std::string url,
                                            std::string body) {
    auto promise = std::make_shared<std::promise<HttpResponse>>();
    auto future = promise->get_future();
    {
        std::lock_guard lk(mutex_);
        if (closing_) {
            promise->set_exception(std::make_exception_ptr(std::runtime_error("scheduler: shutting down")));
            return future;
        }
        queues_[static_cast<int>(priority)].push_back(
            {method, std::move(url), std::move(body), std::move(promise), priority, 0, {}});
    }
    wake_.notify_all();
    return future;
}

void Scheduler::shutdown() {
    std::lock_guard lk(mutex_);
    closing_ = true;
    for (auto& q : queues_) {
        for (auto& p : q)
            p.promise->set_exception(std::make_exception_ptr(std::runtime_error("scheduler: shutting down")));
        q.clear();
    }
}

std::optional<Clock::TimePoint> Scheduler::pump() {
    std::lock_guard lk(mutex_);
    return pumpLocked();
}

std::optional<Clock::TimePoint> Scheduler::pumpLocked() {
    bool progress;
    do {
        progress = collectLocked();
        progress |= dispatchLocked();
    } while (progress);

    const auto now = clock_.now();
    std::optional<Clock::TimePoint> wake;
    auto consider = [&](Clock::TimePoint t) {
        if (!wake || t < *wake) wake = t;
    };
    if (!inFlight_.empty()) consider(now + kPollInterval);
    bool blocked = false;
    for (const auto& q : queues_) {
        for (const auto& p : q) {
            if (p.notBefore > now)
                consider(p.notBefore);
            else
                blocked = true;
        }
    }
    if (blocked && !sent_.empty()) consider(sent_.front() + limit_.window);
    return wake;
}

bool Scheduler::collectLocked() {
    bool progress = false;
    for (auto it = inFlight_.begin(); it != inFlight_.end();) {
        if (it->response.wait_for(0ms) != std::future_status::ready) {
            ++it;
            continue;
        }
        progress = true;
        Pending p = std::move(it->pending);
        auto response = std::move(it->response);
        it = inFlight_.erase(it);
        try {
            HttpResponse r = response.get();
            if (r.status == 429 && p.retries < limit_.maxRetries) {
                p.notBefore = clock_.now() + limit_.retryBase * (1 << p.retries);
                ++p.retries;
                queues_[static_cast<int>(p.priority)].push_front(std::move(p));
            } else {
                p.promise->set_value(std::move(r));
            }
        } catch (...) {
            p.promise->set_exception(std::current_exception());
        }
    }
    return progress;
}

bool Scheduler::dispatchLocked() {
    const auto now = clock_.now();
    while (!sent_.empty() && sent_.front() + limit_.window <= now) sent_.pop_front();

    bool dispatched = false;
    while (static_cast<int>(sent_.size()) < limit_.maxRequests) {
        std::deque<Pending>* from = nullptr;
        std::deque<Pending>::iterator pick;
        for (auto& q : queues_) {
            pick = std::find_if(q.begin(), q.end(),
                                [&](const Pending& p) { return p.notBefore <= now; });
            if (pick != q.end()) {
                from = &q;
                break;
            }
        }
        if (!from) break;

        Pending p = std::move(*pick);
        from->erase(pick);
        dispatched = true;
        sent_.push_back(now);
        try {
            auto response = http_.request(p.method, p.url, p.body);
            inFlight_.push_back({std::move(p), std::move(response)});
        } catch (...) {
            p.promise->set_exception(std::current_exception());
        }
    }
    return dispatched;
}

void Scheduler::run() {
    std::unique_lock lk(mutex_);
    while (!stop_) {
        auto wake = pumpLocked();
        if (stop_) break;
        if (!wake) {
            wake_.wait(lk);
        } else {
            auto d = std::chrono::duration_cast<std::chrono::milliseconds>(*wake - clock_.now());
            wake_.wait_for(lk, std::max(d, 1ms));
        }
    }
}

}  // namespace cardinal
