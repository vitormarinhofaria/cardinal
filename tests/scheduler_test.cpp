#include <gtest/gtest.h>

#include <chrono>

#include "cardinal/scheduler.h"
#include "fakes.h"

using namespace cardinal;
using namespace cardinal::testing;
using namespace std::chrono_literals;

namespace {

RateLimit limit(int max, std::chrono::milliseconds window = 1000ms) {
    RateLimit l;
    l.maxRequests = max;
    l.window = window;
    l.retryBase = 1000ms;
    l.maxRetries = 4;
    return l;
}

bool ready(std::future<HttpResponse>& f) {
    return f.wait_for(0ms) == std::future_status::ready;
}

}  // namespace

TEST(Scheduler, InteractiveJumpsQueue) {
    FakeClock clock;
    FakeHttp http;
    Scheduler s(http, limit(1), clock, false);
    auto bg1 = s.submit(Priority::Background, Method::Get, "bg1");
    auto bg2 = s.submit(Priority::Background, Method::Get, "bg2");
    auto vis = s.submit(Priority::Visible, Method::Get, "vis");
    auto act = s.submit(Priority::Interactive, Method::Get, "act");

    s.pump();
    ASSERT_EQ(http.calls.size(), 1u);
    EXPECT_EQ(http.calls[0].url, "act");

    clock.advance(1000ms);
    s.pump();
    clock.advance(1000ms);
    s.pump();
    clock.advance(1000ms);
    s.pump();
    ASSERT_EQ(http.calls.size(), 4u);
    EXPECT_EQ(http.calls[1].url, "vis");
    EXPECT_EQ(http.calls[2].url, "bg1");
    EXPECT_EQ(http.calls[3].url, "bg2");
    EXPECT_TRUE(ready(act));
}

TEST(Scheduler, NeverExceedsWindow) {
    FakeClock clock;
    FakeHttp http;
    Scheduler s(http, limit(3), clock, false);
    for (int i = 0; i < 7; ++i) s.submit(Priority::Visible, Method::Get, "u" + std::to_string(i));

    s.pump();
    EXPECT_EQ(http.calls.size(), 3u);
    clock.advance(999ms);
    auto wake = s.pump();
    EXPECT_EQ(http.calls.size(), 3u);
    ASSERT_TRUE(wake.has_value());
    EXPECT_EQ(*wake - clock.now(), 1ms);
    clock.advance(1ms);
    s.pump();
    EXPECT_EQ(http.calls.size(), 6u);
    clock.advance(1000ms);
    auto idle = s.pump();
    EXPECT_EQ(http.calls.size(), 7u);
    EXPECT_FALSE(idle.has_value() && *idle <= clock.now());
}

TEST(Scheduler, RetriesAfter429WithBackoff) {
    FakeClock clock;
    FakeHttp http;
    http.script = {{429, ""}, {429, ""}, {200, "ok"}};
    Scheduler s(http, limit(100), clock, false);
    auto f = s.submit(Priority::Visible, Method::Get, "u");

    s.pump();
    EXPECT_EQ(http.calls.size(), 1u);
    EXPECT_FALSE(ready(f));
    clock.advance(999ms);
    s.pump();
    EXPECT_EQ(http.calls.size(), 1u);  // first backoff is 1000 ms
    clock.advance(1ms);
    s.pump();
    EXPECT_EQ(http.calls.size(), 2u);
    clock.advance(1999ms);
    s.pump();
    EXPECT_EQ(http.calls.size(), 2u);  // second backoff doubles to 2000 ms
    clock.advance(1ms);
    s.pump();
    ASSERT_EQ(http.calls.size(), 3u);
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "ok");
}

TEST(Scheduler, ExhaustedRetriesReturn429) {
    FakeClock clock;
    FakeHttp http;
    for (int i = 0; i < 10; ++i) http.script.push_back({429, "limited"});
    RateLimit l = limit(100);
    l.maxRetries = 2;
    Scheduler s(http, l, clock, false);
    auto f = s.submit(Priority::Visible, Method::Get, "u");
    for (int i = 0; i < 10; ++i) {
        s.pump();
        clock.advance(5000ms);
    }
    EXPECT_EQ(http.calls.size(), 3u);  // first try + 2 retries
    ASSERT_TRUE(ready(f));
    EXPECT_EQ(f.get().status, 429);
}

TEST(Scheduler, Http401IsSurfacedNotRetried) {
    FakeClock clock;
    FakeHttp http;
    http.script = {{401, "invalid token"}};
    Scheduler s(http, limit(100), clock, false);
    auto f = s.submit(Priority::Interactive, Method::Get, "u");
    s.pump();
    clock.advance(60000ms);
    s.pump();
    EXPECT_EQ(http.calls.size(), 1u);
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    EXPECT_EQ(r.status, 401);
    EXPECT_EQ(r.body, "invalid token");
}

TEST(Scheduler, TransportErrorPropagatesAndIsNotRetried) {
    FakeClock clock;
    FakeHttp http;
    http.throwNext = {true};
    Scheduler s(http, limit(100), clock, false);
    auto f = s.submit(Priority::Visible, Method::Get, "u");
    s.pump();
    clock.advance(60000ms);
    s.pump();
    EXPECT_EQ(http.calls.size(), 1u);
    ASSERT_TRUE(ready(f));
    EXPECT_THROW(f.get(), std::runtime_error);
}

TEST(Scheduler, ForwardsMethodAndBody) {
    FakeClock clock;
    FakeHttp http;
    Scheduler s(http, limit(100), clock, false);
    s.submit(Priority::Interactive, Method::Post, "u", "{\"a\":1}");
    s.pump();
    ASSERT_EQ(http.calls.size(), 1u);
    EXPECT_EQ(http.calls[0].method, Method::Post);
    EXPECT_EQ(http.calls[0].body, "{\"a\":1}");
}

TEST(Scheduler, WorkerThreadCompletesRequestsWithRealClock) {
    SystemClock clock;
    FakeHttp http;
    Scheduler s(http, RateLimit{}, clock);
    auto f = s.submit(Priority::Interactive, Method::Get, "u");
    ASSERT_EQ(f.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(f.get().status, 200);
}

TEST(Scheduler, DestructorWithPendingRequestsDoesNotHang) {
    SystemClock clock;
    FakeHttp http;
    RateLimit l;
    l.maxRequests = 1;
    l.window = 60000ms;
    std::future<HttpResponse> second;
    {
        Scheduler s(http, l, clock);
        s.submit(Priority::Visible, Method::Get, "a");
        second = s.submit(Priority::Visible, Method::Get, "b");
    }
    EXPECT_THROW(second.get(), std::future_error);  // broken promise: dropped at shutdown
}

TEST(Scheduler, ShutdownFailsQueuedRequestsAndRefusesNewOnes) {
    FakeClock clock;
    FakeHttp http;
    Scheduler s(http, limit(1), clock, false);
    auto first = s.submit(Priority::Visible, Method::Get, "a");
    auto second = s.submit(Priority::Background, Method::Get, "b");
    s.pump();  // "a" goes out, "b" waits for the window
    s.shutdown();
    EXPECT_TRUE(first.wait_for(0ms) == std::future_status::ready);  // answered before shutdown
    ASSERT_EQ(second.wait_for(0ms), std::future_status::ready);
    EXPECT_THROW(second.get(), std::runtime_error);

    auto late = s.submit(Priority::Interactive, Method::Get, "c");
    ASSERT_EQ(late.wait_for(0ms), std::future_status::ready);
    EXPECT_THROW(late.get(), std::runtime_error);
    clock.advance(10000ms);
    s.pump();
    EXPECT_EQ(http.calls.size(), 1u);  // nothing else was sent
}
