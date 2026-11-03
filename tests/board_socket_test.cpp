#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#include "cardinal/board_socket.h"

using namespace cardinal;
using namespace std::chrono_literals;

namespace {

struct Wire {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> inbox;
    std::vector<std::string> sent;
    bool broken = false;
    std::string subscribeReply = R"("result":true)";  // what Trello answers a subscribe with

    void push(std::string m) {
        {
            std::lock_guard lk(mu);
            inbox.push_back(std::move(m));
        }
        cv.notify_all();
    }
    void breakIt() {
        {
            std::lock_guard lk(mu);
            broken = true;
        }
        cv.notify_all();
    }
    std::vector<std::string> sentCopy() {
        std::lock_guard lk(mu);
        return sent;
    }
};

struct FakeConnection : WebSocketConnection {
    explicit FakeConnection(std::shared_ptr<Wire> w) : wire(std::move(w)) {}
    void send(const std::string& text) override {
        std::lock_guard lk(wire->mu);
        if (wire->broken) throw std::runtime_error("broken");
        wire->sent.push_back(text);
        auto j = nlohmann::json::parse(text);
        if (j["type"] == "subscribe")
            wire->inbox.push_back("{\"reqid\":" + std::to_string(j["reqid"].get<int>()) + "," +
                                  wire->subscribeReply + "}");
        wire->cv.notify_all();
    }
    std::optional<std::string> receive(std::chrono::milliseconds timeout) override {
        std::unique_lock lk(wire->mu);
        wire->cv.wait_for(lk, timeout, [&] { return wire->broken || !wire->inbox.empty(); });
        if (wire->broken) throw std::runtime_error("broken");
        if (wire->inbox.empty()) return std::nullopt;
        auto m = std::move(wire->inbox.front());
        wire->inbox.pop_front();
        return m;
    }
    std::shared_ptr<Wire> wire;
};

struct FakeConnector : WebSocketConnector {
    std::unique_ptr<WebSocketConnection> connect(const std::string& url,
                                                 const std::vector<std::string>& headers,
                                                 std::function<bool()>) override {
        std::lock_guard lk(mu);
        ++attempts;
        lastUrl = url;
        lastHeaders = headers;
        if (failNext > 0) {
            --failNext;
            throw std::runtime_error("refused");
        }
        wires.push_back(std::make_shared<Wire>());
        wires.back()->subscribeReply = subscribeReply;
        return std::make_unique<FakeConnection>(wires.back());
    }
    std::shared_ptr<Wire> wire(size_t i) {
        std::lock_guard lk(mu);
        return i < wires.size() ? wires[i] : nullptr;
    }
    int attemptCount() {
        std::lock_guard lk(mu);
        return attempts;
    }

    std::mutex mu;
    int attempts = 0;
    int failNext = 0;
    std::string subscribeReply = R"("result":true)";
    std::string lastUrl;
    std::vector<std::string> lastHeaders;
    std::vector<std::shared_ptr<Wire>> wires;
};

template <class F>
bool eventually(F cond, std::chrono::milliseconds limit = 2000ms) {
    auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return cond();
}

bool contains(const std::vector<std::string>& v, const std::string& a, const std::string& b) {
    for (const auto& s : v)
        if (s.find(a) != std::string::npos && s.find(b) != std::string::npos) return true;
    return false;
}

class BoardSocketTest : public ::testing::Test {
protected:
    void SetUp() override {
        opts.url = "wss://example.test/socket";
        opts.token = "TOK";
        opts.connector = &connector;
        opts.quiet = 50ms;
        opts.maxDelay = 300ms;
        opts.tick = 10ms;
        opts.minBackoff = 10ms;
        opts.maxBackoff = 40ms;
        opts.pingEvery = 1h;
        opts.onActivity = [this](const std::string& b) {
            std::lock_guard lk(mu);
            activity.push_back(b);
        };
        opts.onConnected = [this](bool c) {
            std::lock_guard lk(mu);
            connected.push_back(c);
        };
    }
    std::vector<std::string> activityCopy() {
        std::lock_guard lk(mu);
        return activity;
    }
    std::vector<bool> connectedCopy() {
        std::lock_guard lk(mu);
        return connected;
    }

    FakeConnector connector;
    BoardSocketOptions opts;
    std::mutex mu;
    std::vector<std::string> activity;
    std::vector<bool> connected;
};

}  // namespace

TEST(BoardSocketMessages, RepliesAreNotNotifications) {
    EXPECT_FALSE(BoardSocket::isNotification(R"({"reqid":3,"result":194})"));
    EXPECT_FALSE(BoardSocket::isNotification(""));
    EXPECT_TRUE(BoardSocket::isNotification(
        R"({"idModelChannel":"b1","notify":{"event":"updateModels","typeName":"Card"}})"));
}

TEST_F(BoardSocketTest, DoesNotConnectUntilABoardIsWatched) {
    BoardSocket socket(opts);
    std::this_thread::sleep_for(60ms);
    EXPECT_EQ(connector.attemptCount(), 0);
}

TEST_F(BoardSocketTest, SubscribesToTheWatchedBoardWithTheToken) {
    BoardSocket socket(opts);
    socket.watch("b1", {});
    ASSERT_TRUE(eventually([&] {
        auto w = connector.wire(0);
        return w && contains(w->sentCopy(), "\"subscribe\"", "\"b1\"");
    }));
    std::lock_guard lk(connector.mu);
    EXPECT_EQ(connector.lastUrl, "wss://example.test/socket?token=TOK");
    EXPECT_NE(std::find(connector.lastHeaders.begin(), connector.lastHeaders.end(), "Cookie: token=TOK"),
              connector.lastHeaders.end());
}

TEST_F(BoardSocketTest, BurstOfNotificationsFiresOnceAndRepliesNever) {
    BoardSocket socket(opts);
    socket.watch("b1", {});
    ASSERT_TRUE(eventually([&] { return connector.wire(0) != nullptr; }));
    auto wire = connector.wire(0);
    wire->push(R"({"reqid":0,"result":1})");
    std::this_thread::sleep_for(120ms);
    EXPECT_TRUE(activityCopy().empty());

    for (int i = 0; i < 5; ++i) wire->push(R"({"notify":{"event":"updateModels"}})");
    ASSERT_TRUE(eventually([&] { return !activityCopy().empty(); }));
    std::this_thread::sleep_for(120ms);
    EXPECT_EQ(activityCopy(), std::vector<std::string>{"b1"});
}

TEST_F(BoardSocketTest, SwitchingBoardsResubscribes) {
    BoardSocket socket(opts);
    socket.watch("b1", {});
    ASSERT_TRUE(eventually([&] { return connector.wire(0) != nullptr; }));
    socket.watch("b2", {});
    auto wire = connector.wire(0);
    ASSERT_TRUE(eventually([&] { return contains(wire->sentCopy(), "\"subscribe\"", "\"b2\""); }));
    EXPECT_TRUE(contains(wire->sentCopy(), "\"unsubscribe\"", "\"b1\""));
    EXPECT_TRUE(activityCopy().empty());  // a fresh board needs no reload from us
}

TEST_F(BoardSocketTest, RetriesAfterFailuresAndCatchesUpAfterReconnecting) {
    connector.failNext = 2;
    BoardSocket socket(opts);
    socket.watch("b1", {});
    ASSERT_TRUE(eventually([&] { return connector.wire(0) != nullptr; }));
    EXPECT_EQ(connector.attemptCount(), 3);
    EXPECT_TRUE(activityCopy().empty());  // first connection: the board was just loaded
    ASSERT_TRUE(eventually([&] { return connectedCopy() == std::vector<bool>{true}; }));

    connector.wire(0)->breakIt();
    ASSERT_TRUE(eventually([&] { return connector.wire(1) != nullptr; }));
    ASSERT_TRUE(eventually([&] { return !activityCopy().empty(); }));
    EXPECT_EQ(activityCopy(), std::vector<std::string>{"b1"});
    EXPECT_TRUE(eventually([&] { return connectedCopy() == std::vector<bool>{true, false, true}; }));
}

TEST_F(BoardSocketTest, StopEndsPromptlyAndSilencesCallbacks) {
    auto socket = std::make_unique<BoardSocket>(opts);
    socket->watch("b1", {});
    ASSERT_TRUE(eventually([&] { return connector.wire(0) != nullptr; }));
    auto start = std::chrono::steady_clock::now();
    socket->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
    size_t before = activityCopy().size();
    connector.wire(0)->push(R"({"notify":{}})");
    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(activityCopy().size(), before);
}

TEST_F(BoardSocketTest, RefusedSubscriptionTurnsTheSocketOffForGood) {
    connector.subscribeReply = R"("error":"unauthorized")";
    BoardSocket socket(opts);
    socket.watch("b1", {});
    ASSERT_TRUE(eventually([&] { return connector.wire(0) != nullptr; }));
    std::this_thread::sleep_for(150ms);  // several backoffs' worth
    EXPECT_EQ(connector.attemptCount(), 1);
    EXPECT_TRUE(connectedCopy().empty());  // never reported healthy: polling stays fast
    EXPECT_TRUE(activityCopy().empty());
}

TEST_F(BoardSocketTest, ReportsConnectedOnlyOnceTheSubscriptionIsAccepted) {
    BoardSocket socket(opts);
    socket.watch("b1", {});
    ASSERT_TRUE(eventually([&] { return connectedCopy() == std::vector<bool>{true}; }));
}
