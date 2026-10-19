#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <map>
#include <sstream>

#include "cardinal/auth_flow.h"
#include "cardinal/trello_api.h"

using namespace cardinal;
using namespace std::chrono_literals;

namespace {

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

std::map<std::string, std::string> queryOf(const std::string& url) {
    std::map<std::string, std::string> out;
    std::stringstream ss(url.substr(url.find('?') + 1));
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        auto eq = pair.find('=');
        out[decode(pair.substr(0, eq))] = decode(pair.substr(eq + 1));
    }
    return out;
}

struct Loopback {
    unsigned short port;
    std::string nonce;
};

Loopback parseReturnUrl(const std::string& authorizeUrl) {
    std::string ret = queryOf(authorizeUrl)["return_url"];  // http://127.0.0.1:PORT/callback?nonce=N
    auto colon = ret.find("127.0.0.1:") + 10;
    return {static_cast<unsigned short>(std::stoi(ret.substr(colon, ret.find('/', colon) - colon))),
            ret.substr(ret.find("nonce=") + 6)};
}

struct Reply {
    int status = 0;
    std::string body;
    std::string headers;
};

// Raw HTTP/1.1 over a plain socket; status 0 means the connection was refused.
Reply http(unsigned short port, const std::string& request) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return {};
    }
    ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    std::string raw;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof buf, 0)) > 0) raw.append(buf, static_cast<size_t>(n));
    ::close(fd);
    Reply r;
    if (raw.size() > 12) r.status = std::stoi(raw.substr(9, 3));
    auto split = raw.find("\r\n\r\n");
    if (split != std::string::npos) {
        r.headers = raw.substr(0, split);
        r.body = raw.substr(split + 4);
    }
    return r;
}

std::string get(const std::string& path, unsigned short port, const std::string& host = "") {
    return "GET " + path + " HTTP/1.1\r\nHost: " + (host.empty() ? "127.0.0.1:" + std::to_string(port) : host) +
           "\r\nConnection: close\r\n\r\n";
}

std::string post(const std::string& path, unsigned short port, const std::string& body,
                 const std::string& host = "") {
    return "POST " + path + " HTTP/1.1\r\nHost: " + (host.empty() ? "127.0.0.1:" + std::to_string(port) : host) +
           "\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

const std::string kToken = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

}  // namespace

TEST(AuthFlow, BuildsAuthorizeUrlWithLoopbackReturn) {
    FakeBrowser browser;
    AuthFlow flow("APIKEY", browser);
    auto token = flow.run();
    ASSERT_FALSE(browser.url.empty());
    EXPECT_EQ(browser.url.rfind("https://trello.com/1/authorize?", 0), 0u);
    auto q = queryOf(browser.url);
    EXPECT_EQ(q["key"], "APIKEY");
    EXPECT_EQ(q["response_type"], "token");
    EXPECT_EQ(q["scope"], "read,write");
    EXPECT_EQ(q["expiration"], "never");
    EXPECT_EQ(q["callback_method"], "fragment");
    EXPECT_EQ(q["name"], "Cardinal");
    auto lb = parseReturnUrl(browser.url);
    EXPECT_GT(lb.port, 0);
    EXPECT_GE(lb.nonce.size(), 32u);
    EXPECT_EQ(q["return_url"], "http://127.0.0.1:" + std::to_string(lb.port) + "/callback?nonce=" + lb.nonce);
}

TEST(AuthFlow, ReturnsTokenPostedByCallbackPage) {
    FakeBrowser browser;
    AuthFlow flow("K", browser);
    auto token = flow.run();
    auto lb = parseReturnUrl(browser.url);

    Reply page = http(lb.port, get("/callback?nonce=" + lb.nonce, lb.port));
    EXPECT_EQ(page.status, 200);
    EXPECT_NE(page.body.find("location.hash"), std::string::npos);
    EXPECT_NE(page.headers.find("no-store"), std::string::npos);
    EXPECT_NE(page.headers.find("Content-Security-Policy"), std::string::npos);
    EXPECT_EQ(token.wait_for(0ms), std::future_status::timeout);  // visiting the page is not enough

    Reply posted = http(lb.port, post("/token?nonce=" + lb.nonce, lb.port, kToken));
    EXPECT_EQ(posted.status, 200);
    ASSERT_EQ(token.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(token.get(), kToken);
}

TEST(AuthFlow, IgnoresRequestsWithoutTheSessionNonceAndKeepsWaiting) {
    FakeBrowser browser;
    AuthFlow flow("K", browser);
    auto token = flow.run();
    auto lb = parseReturnUrl(browser.url);

    EXPECT_EQ(http(lb.port, post("/token", lb.port, kToken)).status, 403);
    EXPECT_EQ(http(lb.port, post("/token?nonce=wrong", lb.port, kToken)).status, 403);
    EXPECT_EQ(http(lb.port, get("/callback?nonce=wrong", lb.port)).status, 403);
    EXPECT_EQ(token.wait_for(50ms), std::future_status::timeout);

    EXPECT_EQ(http(lb.port, post("/token?nonce=" + lb.nonce, lb.port, kToken)).status, 200);
    ASSERT_EQ(token.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(token.get(), kToken);
}

TEST(AuthFlow, RejectsMalformedTokenBodies) {
    FakeBrowser browser;
    AuthFlow flow("K", browser);
    auto token = flow.run();
    auto lb = parseReturnUrl(browser.url);
    for (const std::string& bad :
         {std::string(""), std::string("<script>alert(1)</script>"), std::string("short"),
          std::string(600, 'a'), std::string("has space and enough length 1234567890")}) {
        EXPECT_EQ(http(lb.port, post("/token?nonce=" + lb.nonce, lb.port, bad)).status, 400) << bad;
    }
    EXPECT_EQ(token.wait_for(50ms), std::future_status::timeout);
}

TEST(AuthFlow, RejectsForeignHostHeaderAgainstDnsRebinding) {
    FakeBrowser browser;
    AuthFlow flow("K", browser);
    auto token = flow.run();
    auto lb = parseReturnUrl(browser.url);
    EXPECT_EQ(http(lb.port, post("/token?nonce=" + lb.nonce, lb.port, kToken, "evil.example:80")).status, 403);
    EXPECT_EQ(http(lb.port, get("/callback?nonce=" + lb.nonce, lb.port, "evil.example")).status, 403);
    EXPECT_EQ(token.wait_for(50ms), std::future_status::timeout);
}

TEST(AuthFlow, UnknownPathsAndMethodsAreRefused) {
    FakeBrowser browser;
    AuthFlow flow("K", browser);
    auto token = flow.run();
    auto lb = parseReturnUrl(browser.url);
    EXPECT_EQ(http(lb.port, get("/favicon.ico", lb.port)).status, 404);
    EXPECT_EQ(http(lb.port, get("/token?nonce=" + lb.nonce, lb.port)).status, 405);
    EXPECT_EQ(http(lb.port, post("/callback?nonce=" + lb.nonce, lb.port, "x")).status, 405);
    EXPECT_EQ(http(lb.port, "garbage\r\n\r\n").status, 400);
}

TEST(AuthFlow, ServerClosesAfterTheToken) {
    FakeBrowser browser;
    AuthFlow flow("K", browser);
    auto token = flow.run();
    auto lb = parseReturnUrl(browser.url);
    http(lb.port, post("/token?nonce=" + lb.nonce, lb.port, kToken));
    token.get();
    for (int i = 0; i < 100 && http(lb.port, get("/", lb.port)).status != 0; ++i)
        std::this_thread::sleep_for(10ms);
    EXPECT_EQ(http(lb.port, get("/", lb.port)).status, 0);  // refused: nothing is listening
}

TEST(AuthFlow, TimesOutAndClosesServer) {
    FakeBrowser browser;
    AuthOptions opts;
    opts.timeout = 1s;
    AuthFlow flow("K", browser, opts);
    auto token = flow.run();
    auto lb = parseReturnUrl(browser.url);
    ASSERT_EQ(token.wait_for(5s), std::future_status::ready);
    try {
        token.get();
        FAIL() << "expected AuthError";
    } catch (const AuthError& e) {
        EXPECT_EQ(e.kind(), AuthError::Kind::TimedOut);
    }
    EXPECT_EQ(http(lb.port, get("/", lb.port)).status, 0);
}

TEST(AuthFlow, DestructorCancelsAPendingFlowPromptly) {
    FakeBrowser browser;
    std::future<std::string> token;
    unsigned short port;
    auto start = std::chrono::steady_clock::now();
    {
        AuthFlow flow("K", browser);
        token = flow.run();
        port = parseReturnUrl(browser.url).port;
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
    ASSERT_EQ(token.wait_for(0ms), std::future_status::ready);
    try {
        token.get();
        FAIL() << "expected AuthError";
    } catch (const AuthError& e) {
        EXPECT_EQ(e.kind(), AuthError::Kind::Cancelled);
    }
    EXPECT_EQ(http(port, get("/", port)).status, 0);
}

TEST(AuthFlow, BrowserFailureFailsTheFlowAndClosesTheServer) {
    FakeBrowser browser;
    browser.fail = true;
    AuthFlow flow("K", browser);
    auto token = flow.run();
    ASSERT_EQ(token.wait_for(2s), std::future_status::ready);
    try {
        token.get();
        FAIL() << "expected AuthError";
    } catch (const AuthError& e) {
        EXPECT_EQ(e.kind(), AuthError::Kind::BrowserFailed);
    }
}

TEST(AuthFlow, PinnedPortIsUsedWhenGiven) {
    int probe = ::socket(AF_INET, SOCK_STREAM, 0);  // find a free port, then release it
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(probe, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    ::getsockname(probe, reinterpret_cast<sockaddr*>(&a), &len);
    unsigned short port = ntohs(a.sin_port);
    ::close(probe);

    FakeBrowser browser;
    AuthOptions opts;
    opts.port = port;
    AuthFlow flow("K", browser, opts);
    auto token = flow.run();
    EXPECT_EQ(parseReturnUrl(browser.url).port, port);
}

TEST(AuthFlow, PinnedPortInUseFailsClearly) {
    FakeBrowser browser1, browser2;
    AuthFlow first("K", browser1);
    auto t1 = first.run();
    AuthOptions opts;
    opts.port = parseReturnUrl(browser1.url).port;
    AuthFlow second("K", browser2, opts);
    auto t2 = second.run();
    ASSERT_EQ(t2.wait_for(1s), std::future_status::ready);
    try {
        t2.get();
        FAIL() << "expected AuthError";
    } catch (const AuthError& e) {
        EXPECT_EQ(e.kind(), AuthError::Kind::ListenFailed);
    }
}

TEST(AuthFlow, RunningTwiceIsRefused) {
    FakeBrowser browser;
    AuthFlow flow("K", browser);
    auto token = flow.run();
    EXPECT_THROW(flow.run(), std::logic_error);
}

TEST(ManualAuthorizeUrl, AsksTrelloToShowTheTokenOnItsOwnPage) {
    auto url = manualAuthorizeUrl("abc123KEY");
    EXPECT_EQ(url.rfind("https://trello.com/1/authorize?", 0), 0u);
    auto q = queryOf(url);
    EXPECT_EQ(q["key"], "abc123KEY");
    EXPECT_EQ(q["response_type"], "token");
    EXPECT_EQ(q["scope"], "read,write");
    EXPECT_EQ(q["expiration"], "never");
    EXPECT_EQ(q["name"], "Cardinal");
    EXPECT_EQ(q.count("return_url"), 0u);      // no redirect: the token is shown to the user
    EXPECT_EQ(q.count("callback_method"), 0u);
}

TEST(ManualAuthorizeUrl, EncodesTheKey) {
    auto url = manualAuthorizeUrl("a&b=c");
    EXPECT_EQ(queryOf(url)["key"], "a&b=c");
}
