#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <map>
#include <sstream>
#include <thread>

#include "cardinal/session.h"
#include "fakes.h"
#include "http_server.h"

using namespace cardinal;
using namespace cardinal::testing;
using namespace std::chrono_literals;

namespace {

struct ThreadSafeUi : UiDispatcher {
    void post(std::function<void()> fn) override {
        std::lock_guard lk(mu);
        queue.push_back(std::move(fn));
    }
    void runAll() {
        for (;;) {
            std::deque<std::function<void()>> batch;
            {
                std::lock_guard lk(mu);
                batch.swap(queue);
            }
            if (batch.empty()) return;
            for (auto& f : batch) f();
        }
    }
    std::mutex mu;
    std::deque<std::function<void()>> queue;
};

class MemoryCredentials : public CredentialStore {
public:
    std::optional<Credentials> load() override {
        std::lock_guard lk(mu);
        return value;
    }
    void save(const Credentials& c) override {
        std::lock_guard lk(mu);
        value = c;
    }
    void clear() override {
        std::lock_guard lk(mu);
        value.reset();
    }
    std::mutex mu;
    std::optional<Credentials> value;
};

struct FakeBrowser : BrowserOpener {
    void open(const std::string& u) override {
        if (fail) throw std::runtime_error("no browser");
        url = u;
    }
    std::string url;
    bool fail = false;
};

std::string decode(const std::string& s) {
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
}

// POSTs the token to the AuthFlow's loopback server, like the callback page would.
void deliverTokenLikeTheBrowser(const std::string& authorizeUrl, const std::string& token) {
    std::string q = authorizeUrl.substr(authorizeUrl.find('?') + 1);
    std::string ret;
    std::stringstream ss(q);
    std::string pair;
    while (std::getline(ss, pair, '&'))
        if (pair.rfind("return_url=", 0) == 0) ret = decode(pair.substr(11));
    auto colon = ret.find("127.0.0.1:") + 10;
    unsigned short port = static_cast<unsigned short>(std::stoi(ret.substr(colon, ret.find('/', colon) - colon)));
    std::string nonce = ret.substr(ret.find("nonce=") + 6);

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
    std::string req = "POST /token?nonce=" + nonce + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                      "\r\nContent-Length: " + std::to_string(token.size()) + "\r\nConnection: close\r\n\r\n" + token;
    ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
    char buf[256];
    while (::recv(fd, buf, sizeof buf, 0) > 0) {}
    ::close(fd);
}

HttpReply fakeTrello(const HttpRequestInfo& r) {
    if (r.query.find("key=KEY") == std::string::npos) return {401, "invalid key"};
    if (r.query.find("token=GOODTOKEN0123456789") == std::string::npos) return {401, "invalid token"};
    if (r.path == "/1/members/me/boards") return {200, R"([{"id":"b1","name":"Produto","closed":false}])"};
    return {200, "[]"};
}

constexpr const char* kGoodToken = "GOODTOKEN0123456789";

class SessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir = std::filesystem::temp_directory_path() /
              ("cardinal-session-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir);
        server = std::make_unique<RoutedServer>(fakeTrello);
        cfg.dataDir = dir;
        cfg.apiKey = "KEY";
        cfg.apiBase = server->url() + "/1";
        cfg.pollInterval = 10s;
    }
    void TearDown() override {
        session.reset();
        server.reset();
        std::filesystem::remove_all(dir);
    }
    void make() { session = std::make_unique<Session>(cfg, creds, browser, ui, auth); }
    template <class F>
    bool settle(F cond) {
        for (int i = 0; i < 1000 && !cond(); ++i) {
            ui.runAll();
            std::this_thread::sleep_for(5ms);
        }
        ui.runAll();
        return cond();
    }
    SessionState::Phase phase() { return session->state().get().phase; }

    std::filesystem::path dir;
    std::unique_ptr<RoutedServer> server;
    AppConfig cfg;
    AuthOptions auth;
    MemoryCredentials creds;
    FakeBrowser browser;
    ThreadSafeUi ui;
    std::unique_ptr<Session> session;
};

}  // namespace

TEST_F(SessionTest, StartsSignedOutWithoutStoredCredentials) {
    make();
    EXPECT_EQ(phase(), SessionState::Phase::Starting);
    ASSERT_TRUE(settle([&] { return phase() != SessionState::Phase::Starting; }));
    EXPECT_EQ(phase(), SessionState::Phase::SignedOut);
    EXPECT_EQ(session->state().get().error, SessionError::None);
    EXPECT_EQ(session->model(), nullptr);
}

TEST_F(SessionTest, RestoresTheSessionFromTheStoredToken) {
    creds.value = Credentials{kGoodToken};
    make();
    ASSERT_TRUE(settle([&] { return phase() == SessionState::Phase::Ready; }));
    ASSERT_NE(session->model(), nullptr);
    session->model()->start();
    ASSERT_TRUE(settle([&] { return server->count("GET", "/1/members/me/boards") == 1; }));
    EXPECT_NE(server->requests()[0].query.find(std::string("token=") + kGoodToken), std::string::npos);
}

TEST_F(SessionTest, LoginWithPastedTokenSavesItAndBecomesReady) {
    make();
    settle([&] { return phase() == SessionState::Phase::SignedOut; });
    session->loginWithToken("  " + std::string(kGoodToken) + " \n");  // pasted with whitespace
    ASSERT_TRUE(settle([&] { return phase() == SessionState::Phase::Ready; }));
    ASSERT_TRUE(settle([&] { return creds.load().has_value(); }));
    EXPECT_EQ(creds.load()->token, kGoodToken);
}

TEST_F(SessionTest, MalformedPastedTokensAreRefused) {
    make();
    settle([&] { return phase() == SessionState::Phase::SignedOut; });
    for (const char* bad : {"", "   ", "short", "has spaces inside the token 1234567890", "<script>alert(1)</script>"}) {
        session->loginWithToken(bad);
        ui.runAll();
        EXPECT_EQ(phase(), SessionState::Phase::SignedOut) << bad;
        EXPECT_EQ(session->state().get().error, SessionError::InvalidToken) << bad;
    }
    EXPECT_FALSE(creds.load().has_value());
}

TEST_F(SessionTest, BrowserLoginCompletesWhenTheCallbackPageDeliversTheToken) {
    make();
    settle([&] { return phase() == SessionState::Phase::SignedOut; });
    session->startLogin();
    EXPECT_EQ(phase(), SessionState::Phase::WaitingForBrowser);
    EXPECT_NE(browser.url.find("https://trello.com/1/authorize?"), std::string::npos);
    EXPECT_NE(browser.url.find("key=KEY"), std::string::npos);

    deliverTokenLikeTheBrowser(browser.url, kGoodToken);
    ASSERT_TRUE(settle([&] { return phase() == SessionState::Phase::Ready; }));
    ASSERT_TRUE(settle([&] { return creds.load().has_value(); }));
    EXPECT_EQ(creds.load()->token, kGoodToken);
}

TEST_F(SessionTest, BrowserLoginTimeoutReturnsToSignedOutWithAnError) {
    auth.timeout = 1s;
    make();
    settle([&] { return phase() == SessionState::Phase::SignedOut; });
    session->startLogin();
    ASSERT_TRUE(settle([&] { return phase() == SessionState::Phase::SignedOut; }));
    EXPECT_EQ(session->state().get().error, SessionError::LoginTimedOut);
}

TEST_F(SessionTest, BrowserThatCannotOpenIsReported) {
    browser.fail = true;
    make();
    settle([&] { return phase() == SessionState::Phase::SignedOut; });
    session->startLogin();
    ASSERT_TRUE(settle([&] { return session->state().get().error == SessionError::BrowserFailed; }));
    EXPECT_EQ(phase(), SessionState::Phase::SignedOut);
}

TEST_F(SessionTest, MissingApiKeyIsReportedBeforeTryingTheBrowser) {
    cfg.apiKey = "";
    make();
    settle([&] { return phase() == SessionState::Phase::SignedOut; });
    session->startLogin();
    EXPECT_EQ(phase(), SessionState::Phase::SignedOut);
    EXPECT_EQ(session->state().get().error, SessionError::MissingApiKey);
    EXPECT_TRUE(browser.url.empty());
}

TEST_F(SessionTest, CancelLoginReturnsToSignedOutWithoutError) {
    make();
    settle([&] { return phase() == SessionState::Phase::SignedOut; });
    session->startLogin();
    session->cancelLogin();
    EXPECT_EQ(phase(), SessionState::Phase::SignedOut);
    EXPECT_EQ(session->state().get().error, SessionError::None);
    ui.runAll();
    EXPECT_EQ(phase(), SessionState::Phase::SignedOut);  // the cancelled flow must not log in later
}

TEST_F(SessionTest, LogoutClearsCredentialsAndDropsTheContext) {
    creds.value = Credentials{kGoodToken};
    make();
    ASSERT_TRUE(settle([&] { return phase() == SessionState::Phase::Ready; }));
    session->logout();
    EXPECT_EQ(phase(), SessionState::Phase::SignedOut);
    ASSERT_TRUE(settle([&] { return !creds.load().has_value() && session->model() == nullptr; }));
}

TEST_F(SessionTest, TokenRejectedByTrelloSignsOutClearsItAndKeepsTheCache) {
    creds.value = Credentials{"WRONGTOKEN0123456789"};
    make();
    ASSERT_TRUE(settle([&] { return phase() == SessionState::Phase::Ready; }));
    session->model()->start();
    ASSERT_TRUE(settle([&] { return phase() == SessionState::Phase::SignedOut; }));
    EXPECT_EQ(session->state().get().error, SessionError::SessionExpired);
    ASSERT_TRUE(settle([&] { return !creds.load().has_value() && session->model() == nullptr; }));
    EXPECT_TRUE(std::filesystem::exists(dir / "cache.db"));
}

TEST_F(SessionTest, SigningInAgainAfterExpiryWorks) {
    creds.value = Credentials{"WRONGTOKEN0123456789"};
    make();
    ASSERT_TRUE(settle([&] { return phase() == SessionState::Phase::Ready; }));
    session->model()->start();
    ASSERT_TRUE(settle([&] { return phase() == SessionState::Phase::SignedOut && session->model() == nullptr; }));
    session->loginWithToken(kGoodToken);
    ASSERT_TRUE(settle([&] { return phase() == SessionState::Phase::Ready; }));
    session->model()->start();
    ASSERT_TRUE(settle([&] { return !session->model()->boardPicker()->state().get().boards.empty(); }));
}

TEST_F(SessionTest, DestroyingWhileWaitingForTheBrowserDoesNotHang) {
    make();
    settle([&] { return phase() == SessionState::Phase::SignedOut; });
    session->startLogin();
    auto start = std::chrono::steady_clock::now();
    session.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 3s);
    ui.runAll();  // late callbacks are harmless
}
