#pragma once

#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "cardinal/http.h"
#include "cardinal/scheduler.h"

namespace cardinal::testing {

class FakeClock : public Clock {
public:
    TimePoint now() override { return t_; }
    void advance(std::chrono::milliseconds d) { t_ += d; }

private:
    TimePoint t_{std::chrono::steady_clock::duration(std::chrono::hours(1))};
};

struct Call {
    Method method;
    std::string url;
    std::string body;
};

// Answers every request immediately with the next scripted response (default 200 "{}").
class FakeHttp : public HttpClient {
public:
    std::future<HttpResponse> request(Method m, std::string url, std::string body) override {
        std::lock_guard lk(mu_);
        calls.push_back({m, url, body});
        std::promise<HttpResponse> p;
        if (!throwNext.empty() && throwNext.front()) {
            throwNext.pop_front();
            p.set_exception(std::make_exception_ptr(std::runtime_error("network down")));
        } else {
            if (!throwNext.empty()) throwNext.pop_front();
            HttpResponse r{200, "{}"};
            if (!script.empty()) {
                r = script.front();
                script.pop_front();
            }
            p.set_value(r);
        }
        return p.get_future();
    }

    std::vector<Call> calls;
    std::deque<HttpResponse> script;
    std::deque<bool> throwNext;  // per call: true => transport failure
    std::mutex mu_;
};

}  // namespace cardinal::testing
