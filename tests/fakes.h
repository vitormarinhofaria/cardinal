#pragma once

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "cardinal/executor.h"
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

namespace cardinal::testing {

struct Submitted {
    Priority priority;
    Method method;
    std::string url;
    std::string body;
};

// Records every submission and answers with the next scripted response (default 200 "{}").
class FakeRequester : public Requester {
public:
    std::future<HttpResponse> submit(Priority p, Method m, std::string url,
                                     std::string body) override {
        calls.push_back({p, m, url, body});
        std::promise<HttpResponse> promise;
        HttpResponse r{200, "{}"};
        if (!script.empty()) {
            r = script.front();
            script.pop_front();
        }
        promise.set_value(r);
        return promise.get_future();
    }

    std::vector<Submitted> calls;
    std::deque<HttpResponse> script;
};

}  // namespace cardinal::testing

namespace cardinal::testing {

struct Route {
    std::string path;      // path below the API base, without query
    std::string contains;  // substring the URL must also contain ("" = any)
    HttpResponse response;
    std::optional<Method> method = std::nullopt;  // nullopt = any method
};

// Answers by route (first match wins); unknown paths get 404. `failAll` simulates being offline.
class RouteRequester : public Requester {
public:
    std::future<HttpResponse> submit(Priority p, Method m, std::string url,
                                     std::string body) override {
        std::lock_guard lk(mu);
        calls.push_back({p, m, url, body});
        std::promise<HttpResponse> promise;
        if (failAll) {
            promise.set_exception(std::make_exception_ptr(std::runtime_error("offline")));
            return promise.get_future();
        }
        const std::string base = "https://api.trello.com/1";
        std::string rest = url.substr(base.size());
        std::string path = rest.substr(0, rest.find('?'));
        HttpResponse r{404, "not found"};
        for (const auto& route : routes) {
            if (route.path == path && (!route.method || *route.method == m) &&
                (route.contains.empty() || url.find(route.contains) != std::string::npos)) {
                r = route.response;
                break;
            }
        }
        promise.set_value(r);
        return promise.get_future();
    }

    size_t countPath(const std::string& path) {
        std::lock_guard lk(mu);
        size_t n = 0;
        for (const auto& c : calls)
            if (c.url.find("https://api.trello.com/1" + path + "?") == 0) ++n;
        return n;
    }

    std::vector<Route> routes;
    std::vector<Submitted> calls;
    bool failAll = false;
    std::mutex mu;
};

}  // namespace cardinal::testing

namespace cardinal::testing {

// Runs jobs only when the test says so, in posting order.
class QueueExecutor : public Executor {
public:
    void post(std::function<void()> job) override {
        std::lock_guard lk(mu);  // timers and workers may post from their own threads
        jobs.push_back(std::move(job));
    }
    bool runOne() {
        std::function<void()> job;
        {
            std::lock_guard lk(mu);
            if (jobs.empty()) return false;
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        job();
        return true;
    }
    void drain() {
        while (runOne()) {}
    }
    std::deque<std::function<void()>> jobs;  // read it only from the test thread, between runs
    std::mutex mu;
};

// "a=b&c=d" (percent-encoded) -> {a:b, c:d}
inline std::map<std::string, std::string> parseForm(const std::string& body) {
    auto decode = [](const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size() + 0) {
                out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else {
                out += s[i];
            }
        }
        return out;
    };
    std::map<std::string, std::string> out;
    std::stringstream ss(body);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        auto eq = pair.find('=');
        out[decode(pair.substr(0, eq))] = eq == std::string::npos ? "" : decode(pair.substr(eq + 1));
    }
    return out;
}

}  // namespace cardinal::testing
