#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "cardinal/event_source.h"
#include "cardinal/time_util.h"
#include "fakes.h"

using namespace cardinal;
using namespace cardinal::testing;
using namespace std::chrono_literals;

namespace {

constexpr const char* kActions = R"([
    {"id":"a2","type":"commentCard","date":"2026-10-19T12:05:00.000Z","idMemberCreator":"m1",
     "data":{"text":"oi","card":{"id":"c1"}}},
    {"id":"a1","type":"updateCard","date":"2026-10-19T12:01:00.000Z","idMemberCreator":"m2",
     "data":{"card":{"id":"c1"}}}])";

class PollingTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("cardinal-poll-" + std::string(::testing::UnitTest::GetInstance()
                                                   ->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        store = std::make_unique<Store>(Store::open(dir_ / "cache.db"));
        opts.autoStart = false;
        opts.nowIso = [] { return std::string("2026-10-19T13:00:00.000Z"); };
        opts.onError = [this](const std::exception& e) { errors.push_back(e.what()); };
    }
    void TearDown() override {
        store.reset();
        std::filesystem::remove_all(dir_);
    }
    std::vector<Submitted> actionCalls(const std::string& board) {
        std::vector<Submitted> out;
        for (const auto& c : req.calls)
            if (c.url.find("/boards/" + board + "/actions?") != std::string::npos) out.push_back(c);
        return out;
    }

    std::filesystem::path dir_;
    std::unique_ptr<Store> store;
    RouteRequester req;
    TrelloApi api{req, "K", "T"};
    PollingOptions opts;
    std::vector<std::string> errors;
};

}  // namespace

TEST(IsoUtc, FormatsLikeTrello) {
    EXPECT_EQ(isoUtc(0), "1970-01-01T00:00:00.000Z");
    EXPECT_EQ(isoUtc(1'760'875'500), "2025-10-19T12:05:00.000Z");
}

TEST_F(PollingTest, UsesStoredCursor) {
    store->setSyncCursor("b1", "2026-10-19T12:00:00.000Z");
    req.routes = {{"/boards/b1/actions", "", {200, "[]"}}};
    PollingEventSource poller(api, *store, opts);
    poller.watch("b1", [](std::vector<Action>) {});
    poller.pollOnce();
    auto calls = actionCalls("b1");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_NE(calls[0].url.find("since=2026-10-19T12%3A00%3A00.000Z"), std::string::npos)
        << calls[0].url;
    EXPECT_EQ(calls[0].priority, Priority::Background);
}

TEST_F(PollingTest, DeliversOldestFirstAndAdvancesCursorAfterApply) {
    store->setSyncCursor("b1", "2026-10-19T12:00:00.000Z");
    req.routes = {{"/boards/b1/actions", "", {200, kActions}}};
    PollingEventSource poller(api, *store, opts);
    std::vector<Action> got;
    poller.watch("b1", [&](std::vector<Action> a) { got = std::move(a); });
    poller.pollOnce();
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].id, "a1");
    EXPECT_EQ(got[1].id, "a2");
    EXPECT_EQ(store->syncCursor("b1"), "2026-10-19T12:05:00.000Z");
}

TEST_F(PollingTest, CursorNotAdvancedWhenHandlerThrowsAndBatchIsRedelivered) {
    store->setSyncCursor("b1", "2026-10-19T12:00:00.000Z");
    req.routes = {{"/boards/b1/actions", "", {200, kActions}}};
    PollingEventSource poller(api, *store, opts);
    int deliveries = 0;
    bool fail = true;
    poller.watch("b1", [&](std::vector<Action> a) {
        ++deliveries;
        EXPECT_EQ(a.size(), 2u);
        if (fail) throw std::runtime_error("refetch failed");
    });
    poller.pollOnce();
    EXPECT_EQ(store->syncCursor("b1"), "2026-10-19T12:00:00.000Z");
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(errors[0].find("refetch failed"), std::string::npos);

    fail = false;
    poller.pollOnce();
    EXPECT_EQ(deliveries, 2);
    EXPECT_EQ(store->syncCursor("b1"), "2026-10-19T12:05:00.000Z");
}

TEST_F(PollingTest, NoActionsMeansNoChange) {
    store->setSyncCursor("b1", "2026-10-19T12:00:00.000Z");
    req.routes = {{"/boards/b1/actions", "", {200, "[]"}}};
    PollingEventSource poller(api, *store, opts);
    int calls = 0;
    poller.watch("b1", [&](std::vector<Action>) { ++calls; });
    poller.pollOnce();
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(store->syncCursor("b1"), "2026-10-19T12:00:00.000Z");
}

TEST_F(PollingTest, MissingCursorBaselinesToNowWithoutFetching) {
    PollingEventSource poller(api, *store, opts);
    poller.watch("b1", [](std::vector<Action>) { FAIL() << "nothing to deliver"; });
    poller.pollOnce();
    EXPECT_TRUE(req.calls.empty());
    EXPECT_EQ(store->syncCursor("b1"), "2026-10-19T13:00:00.000Z");
}

TEST_F(PollingTest, StopsWhenBoardUnwatched) {
    store->setSyncCursor("b1", "2026-10-19T12:00:00.000Z");
    PollingEventSource poller(api, *store, opts);
    poller.watch("b1", [](std::vector<Action>) {});
    poller.unwatch("b1");
    poller.pollOnce();
    EXPECT_TRUE(req.calls.empty());
}

TEST_F(PollingTest, EachBoardHasItsOwnCursorAndErrorsDoNotBlockOthers) {
    store->setSyncCursor("b1", "2026-10-19T12:00:00.000Z");
    store->setSyncCursor("b2", "2026-10-19T11:00:00.000Z");
    req.routes = {{"/boards/b1/actions", "", {401, "invalid token"}},
                  {"/boards/b2/actions", "", {200, kActions}}};
    PollingEventSource poller(api, *store, opts);
    int b2Batches = 0;
    poller.watch("b1", [](std::vector<Action>) { FAIL(); });
    poller.watch("b2", [&](std::vector<Action>) { ++b2Batches; });
    poller.pollOnce();
    EXPECT_EQ(errors.size(), 1u);
    EXPECT_EQ(b2Batches, 1);
    EXPECT_EQ(store->syncCursor("b1"), "2026-10-19T12:00:00.000Z");
    EXPECT_EQ(store->syncCursor("b2"), "2026-10-19T12:05:00.000Z");
}

TEST_F(PollingTest, TransportFailureIsReportedNotThrown) {
    store->setSyncCursor("b1", "2026-10-19T12:00:00.000Z");
    req.failAll = true;
    PollingEventSource poller(api, *store, opts);
    poller.watch("b1", [](std::vector<Action>) { FAIL(); });
    EXPECT_NO_THROW(poller.pollOnce());
    EXPECT_EQ(errors.size(), 1u);
}

TEST_F(PollingTest, BackgroundThreadPollsPeriodicallyAndStopsOnDestruction) {
    store->setSyncCursor("b1", "2026-10-19T12:00:00.000Z");
    req.routes = {{"/boards/b1/actions", "", {200, "[]"}}};
    opts.autoStart = true;
    opts.interval = 20ms;
    {
        PollingEventSource poller(api, *store, opts);
        poller.watch("b1", [](std::vector<Action>) {});
        for (int i = 0; i < 200 && req.countPath("/boards/b1/actions") < 3; ++i)
            std::this_thread::sleep_for(10ms);
        EXPECT_GE(req.countPath("/boards/b1/actions"), 3u);
    }
    size_t after = req.countPath("/boards/b1/actions");
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(req.countPath("/boards/b1/actions"), after);  // destructor stopped the thread
}
