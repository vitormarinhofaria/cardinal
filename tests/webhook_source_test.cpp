#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

#include "cardinal/cloudflared.h"
#include "cardinal/webhook_source.h"

using namespace cardinal;
using namespace std::chrono_literals;
using nlohmann::json;

namespace {

std::map<std::string, std::string> form(const std::string& body) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < body.size()) {
        auto amp = body.find('&', pos);
        auto pair = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        auto eq = pair.find('=');
        std::string v = pair.substr(eq + 1), dec;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == '%' && i + 2 < v.size()) {
                dec += static_cast<char>(std::stoi(v.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else {
                dec += v[i] == '+' ? ' ' : v[i];
            }
        }
        out[pair.substr(0, eq)] = dec;
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}

// Trello's webhook endpoints, in memory.
struct FakeTrello : Requester {
    std::future<HttpResponse> submit(Priority, Method m, std::string url, std::string body) override {
        std::lock_guard lk(mu);
        std::string path = url.substr(0, url.find('?'));
        path = path.substr(path.find("/1/") + 2);
        calls.push_back((m == Method::Get ? "GET " : m == Method::Post ? "POST " : m == Method::Put ? "PUT " : "DELETE ") + path);
        std::promise<HttpResponse> p;
        p.set_value(handle(m, path, form(body)));
        return p.get_future();
    }
    HttpResponse handle(Method m, const std::string& path, std::map<std::string, std::string> f) {
        if (refuse) return {401, "invalid token"};
        if (m == Method::Get && path.rfind("/tokens/", 0) == 0) {
            json a = json::array();
            for (const auto& [id, w] : hooks) a.push_back(w);
            return {200, a.dump()};
        }
        if ((m == Method::Post || m == Method::Put) && validationFailures > 0) {
            --validationFailures;
            return {400, "URL (callbackURL) did not return 200 status code"};
        }
        if (m == Method::Post && path == "/webhooks") {
            std::string id = "w" + std::to_string(++seq);
            hooks[id] = {{"id", id}, {"description", f["description"]}, {"callbackURL", f["callbackURL"]},
                         {"idModel", f["idModel"]}, {"active", true}};
            return {200, hooks[id].dump()};
        }
        if (path.rfind("/webhooks/", 0) == 0) {
            std::string id = path.substr(10);
            if (!hooks.count(id)) return {404, "not found"};
            if (m == Method::Delete) {
                hooks.erase(id);
                return {200, "{}"};
            }
            hooks[id]["callbackURL"] = f["callbackURL"];
            hooks[id]["idModel"] = f["idModel"];
            return {200, hooks[id].dump()};
        }
        return {404, "not found"};
    }
    std::vector<std::string> callsCopy() {
        std::lock_guard lk(mu);
        return calls;
    }
    json hook(const std::string& id) {
        std::lock_guard lk(mu);
        return hooks.count(id) ? hooks[id] : json();
    }
    size_t hookCount() {
        std::lock_guard lk(mu);
        return hooks.size();
    }

    std::mutex mu;
    std::map<std::string, json> hooks;
    std::vector<std::string> calls;
    int seq = 0;
    int validationFailures = 0;
    bool refuse = false;
};

struct FakeTunnel : Tunnel {
    explicit FakeTunnel(std::atomic<int>& opens, std::atomic<bool>& up) : opens(opens), up(up) {}
    std::string open(unsigned short, std::function<bool()>) override {
        up = true;
        return "https://t" + std::to_string(++opens) + ".trycloudflare.com";
    }
    bool alive() override { return up; }
    void close() override { up = false; }
    std::atomic<int>& opens;
    std::atomic<bool>& up;
};

int post(unsigned short port, const std::string& method, const std::string& path) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        ::close(fd);
        return -1;
    }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: x.trycloudflare.com\r\nContent-Length: 2\r\n\r\n{}";
    ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
    char buf[256] = {};
    ssize_t n = ::recv(fd, buf, sizeof buf - 1, 0);
    ::close(fd);
    if (n < 12) return -1;
    return std::atoi(buf + 9);
}

template <class F>
bool eventually(F cond, std::chrono::milliseconds limit = 3000ms) {
    auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return cond();
}

class WebhookTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir = std::filesystem::temp_directory_path() /
              ("cardinal-hook-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir);
        opts.api = &api;
        opts.dataDir = dir;
        opts.makeTunnel = [this] { return std::make_unique<FakeTunnel>(opens, up); };
        opts.quiet = 50ms;
        opts.maxDelay = 300ms;
        opts.tick = 10ms;
        opts.minBackoff = 10ms;
        opts.maxBackoff = 40ms;
        opts.validationRetry = 10ms;
        opts.validationWindow = 2s;
        opts.onActivity = [this](const std::string& b) {
            std::lock_guard lk(mu);
            activity.push_back(b);
        };
        opts.onConnected = [this](bool c) {
            std::lock_guard lk(mu);
            connected.push_back(c);
        };
    }
    void TearDown() override { std::filesystem::remove_all(dir); }

    std::vector<std::string> activityCopy() {
        std::lock_guard lk(mu);
        return activity;
    }
    std::vector<bool> connectedCopy() {
        std::lock_guard lk(mu);
        return connected;
    }
    bool healthy() {
        auto c = connectedCopy();
        return !c.empty() && c.back();
    }
    std::string installId() {
        std::string id;
        std::getline(std::ifstream(dir / "install-id"), id);
        return id;
    }

    std::filesystem::path dir;
    FakeTrello req;
    TrelloApi api{req, "K", "TOKEN"};
    std::atomic<int> opens{0};
    std::atomic<bool> up{false};
    WebhookOptions opts;
    std::mutex mu;
    std::vector<std::string> activity;
    std::vector<bool> connected;
};

}  // namespace

TEST_F(WebhookTest, NothingHappensUntilABoardIsWatched) {
    WebhookEventSource src(opts);
    std::this_thread::sleep_for(60ms);
    EXPECT_EQ(opens, 0);
    EXPECT_TRUE(req.callsCopy().empty());
}

TEST_F(WebhookTest, CreatesAWebhookOnTheSecretPathAndDeletesItOnStop) {
    auto src = std::make_unique<WebhookEventSource>(opts);
    src->watch("b1", {});
    ASSERT_TRUE(eventually([&] { return healthy(); }));
    ASSERT_EQ(req.hookCount(), 1u);
    auto w = req.hook("w1");
    EXPECT_EQ(w["idModel"], "b1");
    EXPECT_EQ(w["callbackURL"], "https://t1.trycloudflare.com" + src->path());
    EXPECT_EQ(w["description"], "Cardinal " + installId());
    EXPECT_EQ(src->path().size(), 6u + 32u);  // "/hook/" + 128 random bits

    src->stop();
    EXPECT_EQ(req.hookCount(), 0u);
    EXPECT_FALSE(up);  // tunnel closed
}

TEST_F(WebhookTest, ReusesThisInstallationsWebhookAndLeavesOthersAlone) {
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "install-id") << "0123456789abcdef\n";
    req.hooks["old"] = {{"id", "old"}, {"description", "Cardinal 0123456789abcdef"},
                        {"callbackURL", "https://gone.trycloudflare.com/hook/x"}, {"idModel", "b9"}};
    req.hooks["dup"] = {{"id", "dup"}, {"description", "Cardinal 0123456789abcdef"},
                        {"callbackURL", "https://gone2.trycloudflare.com/hook/y"}, {"idModel", "b9"}};
    req.hooks["other"] = {{"id", "other"}, {"description", "Cardinal ffffffffffffffff"},
                          {"callbackURL", "https://laptop.trycloudflare.com/hook/z"}, {"idModel", "b1"}};
    auto src = std::make_unique<WebhookEventSource>(opts);
    src->watch("b1", {});
    ASSERT_TRUE(eventually([&] { return healthy(); }));
    // Exactly one of ours is left (the duplicate is removed), repointed at the new tunnel + board.
    const bool keptOld = !req.hook("old").is_null();
    EXPECT_NE(keptOld, !req.hook("dup").is_null());
    auto kept = req.hook(keptOld ? "old" : "dup");
    EXPECT_EQ(kept["idModel"], "b1");
    EXPECT_EQ(kept["callbackURL"], "https://t1.trycloudflare.com" + src->path());
    for (const auto& c : req.callsCopy()) EXPECT_NE(c, "POST /webhooks");
    src->stop();
    EXPECT_EQ(req.hook("other")["idModel"], "b1");  // another machine's: untouched
}

TEST_F(WebhookTest, DeliveriesFireOnceAndOnlyOnTheSecretPath) {
    WebhookEventSource src(opts);
    src.watch("b1", {});
    ASSERT_TRUE(eventually([&] { return healthy(); }));
    EXPECT_EQ(post(src.port(), "HEAD", src.path()), 200);  // Trello's check
    EXPECT_EQ(post(src.port(), "POST", "/hook/guess"), 404);
    std::this_thread::sleep_for(100ms);
    EXPECT_TRUE(activityCopy().empty());

    for (int i = 0; i < 5; ++i) EXPECT_EQ(post(src.port(), "POST", src.path()), 200);
    ASSERT_TRUE(eventually([&] { return !activityCopy().empty(); }));
    std::this_thread::sleep_for(120ms);
    EXPECT_EQ(activityCopy(), std::vector<std::string>{"b1"});
}

TEST_F(WebhookTest, SwitchingBoardsRepointsTheSameWebhook) {
    WebhookEventSource src(opts);
    src.watch("b1", {});
    ASSERT_TRUE(eventually([&] { return healthy(); }));
    src.watch("b2", {});
    ASSERT_TRUE(eventually([&] { return req.hook("w1")["idModel"] == "b2"; }));
    EXPECT_EQ(req.hookCount(), 1u);
    EXPECT_TRUE(activityCopy().empty());
}

TEST_F(WebhookTest, RetriesWhileTrelloCannotReachTheNewTunnelYet) {
    req.validationFailures = 3;
    WebhookEventSource src(opts);
    src.watch("b1", {});
    ASSERT_TRUE(eventually([&] { return healthy(); }));
    EXPECT_EQ(req.hookCount(), 1u);
}

TEST_F(WebhookTest, TunnelLossRepointsToTheNewUrlAndCatchesUp) {
    WebhookEventSource src(opts);
    src.watch("b1", {});
    ASSERT_TRUE(eventually([&] { return healthy(); }));
    up = false;  // cloudflared died
    ASSERT_TRUE(eventually([&] {
        return req.hook("w1")["callbackURL"] == "https://t2.trycloudflare.com" + src.path();
    }));
    ASSERT_TRUE(eventually([&] { return !activityCopy().empty(); }));
    EXPECT_EQ(activityCopy(), std::vector<std::string>{"b1"});
    EXPECT_TRUE(eventually([&] { return connectedCopy() == std::vector<bool>{true, false, true}; }));
}

TEST_F(WebhookTest, RefusedTokenTurnsWebhooksOffWithoutClaimingSuccess) {
    req.refuse = true;
    WebhookEventSource src(opts);
    src.watch("b1", {});
    ASSERT_TRUE(eventually([&] { return !req.callsCopy().empty(); }));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(req.callsCopy().size(), 1u);  // no retry storm
    EXPECT_TRUE(connectedCopy().empty());
}

TEST_F(WebhookTest, MissingCloudflaredTurnsWebhooksOff) {
    std::atomic<int> attempts{0};
    opts.makeTunnel = [&]() -> std::unique_ptr<Tunnel> {
        ++attempts;
        throw CloudflaredUnavailable("no cloudflared build for riscv64");
    };
    WebhookEventSource src(opts);
    src.watch("b1", {});
    ASSERT_TRUE(eventually([&] { return attempts == 1; }));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(attempts, 1);
    EXPECT_TRUE(req.callsCopy().empty());
}

TEST_F(WebhookTest, StopIsPromptEvenWhenTrelloNeverAnswers) {
    struct Silent : Requester {
        std::future<HttpResponse> submit(Priority, Method, std::string, std::string) override {
            promises.emplace_back();
            return promises.back().get_future();
        }
        std::deque<std::promise<HttpResponse>> promises;
    } silent;
    TrelloApi silentApi{silent, "K", "T"};
    opts.api = &silentApi;
    auto src = std::make_unique<WebhookEventSource>(opts);
    src->watch("b1", {});
    ASSERT_TRUE(eventually([&] { return opens == 1; }));
    std::this_thread::sleep_for(30ms);
    auto start = std::chrono::steady_clock::now();
    src.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    for (auto& p : silent.promises) p.set_value({500, ""});  // let the detached waits finish
    std::this_thread::sleep_for(20ms);
}
