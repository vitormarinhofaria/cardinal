#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

#include "cardinal/http.h"
#include "http_server.h"

using namespace cardinal;
using namespace std::chrono_literals;

namespace {

// One-shot HTTP/1.1 server on a loopback port. Records the raw request and replies with `reply`.
class LoopbackServer {
public:
    explicit LoopbackServer(std::string reply) : reply_(std::move(reply)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        socklen_t len = sizeof addr;
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::listen(fd_, 1);
        thread_ = std::thread([this] { serve(); });
    }
    ~LoopbackServer() {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        thread_.join();
    }
    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }
    std::string request() const { return request_; }

private:
    void serve() {
        int c = ::accept(fd_, nullptr, nullptr);
        if (c < 0) return;
        std::string req;
        char buf[4096];
        size_t headerEnd = std::string::npos;
        size_t want = 0;
        while (true) {
            ssize_t n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
            if (headerEnd == std::string::npos) {
                headerEnd = req.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    auto cl = req.find("Content-Length: ");
                    want = headerEnd + 4 +
                           (cl == std::string::npos ? 0 : std::stoul(req.substr(cl + 16)));
                }
            }
            if (headerEnd != std::string::npos && req.size() >= want) break;
        }
        request_ = req;
        ::send(c, reply_.data(), reply_.size(), MSG_NOSIGNAL);
        ::close(c);
    }

    std::string reply_;
    int fd_ = -1;
    uint16_t port_ = 0;
    std::string request_;
    std::thread thread_;
};

std::string reply(int status, const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

}  // namespace

TEST(CurlHttp, GetReturnsStatusAndBody) {
    LoopbackServer server(reply(200, "{\"ok\":true} café"));
    auto http = makeCurlHttpClient(1);
    auto f = http->request(Method::Get, server.url("/1/boards/x?fields=name"), "");
    ASSERT_EQ(f.wait_for(5s), std::future_status::ready);
    HttpResponse r = f.get();
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "{\"ok\":true} café");
    EXPECT_EQ(server.request().rfind("GET /1/boards/x?fields=name HTTP/1.1", 0), 0u);
}

TEST(CurlHttp, PostSendsBody) {
    LoopbackServer server(reply(200, "{}"));
    auto http = makeCurlHttpClient(1);
    auto f = http->request(Method::Post, server.url("/1/cards"), "name=Caf%C3%A9");
    ASSERT_EQ(f.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(f.get().status, 200);
    EXPECT_EQ(server.request().rfind("POST /1/cards HTTP/1.1", 0), 0u);
    EXPECT_NE(server.request().find("name=Caf%C3%A9"), std::string::npos);
}

TEST(CurlHttp, PutAndDeleteUseTheirMethods) {
    {
        LoopbackServer server(reply(200, "{}"));
        auto http = makeCurlHttpClient(1);
        auto f = http->request(Method::Put, server.url("/1/cards/c1"), "desc=x");
        ASSERT_EQ(f.wait_for(5s), std::future_status::ready);
        f.get();
        EXPECT_EQ(server.request().rfind("PUT /1/cards/c1 ", 0), 0u);
    }
    {
        LoopbackServer server(reply(200, "{}"));
        auto http = makeCurlHttpClient(1);
        auto f = http->request(Method::Delete, server.url("/1/cards/c1"), "");
        ASSERT_EQ(f.wait_for(5s), std::future_status::ready);
        f.get();
        EXPECT_EQ(server.request().rfind("DELETE /1/cards/c1 ", 0), 0u);
    }
}

TEST(CurlHttp, ErrorStatusIsAResponseNotAnException) {
    LoopbackServer server(reply(401, "invalid token"));
    auto http = makeCurlHttpClient(1);
    auto f = http->request(Method::Get, server.url("/"), "");
    ASSERT_EQ(f.wait_for(5s), std::future_status::ready);
    HttpResponse r = f.get();
    EXPECT_EQ(r.status, 401);
    EXPECT_EQ(r.body, "invalid token");
}

TEST(CurlHttp, ConnectionRefusedIsAnException) {
    uint16_t port;
    {
        LoopbackServer probe(reply(200, ""));
        port = static_cast<uint16_t>(std::stoi(probe.url("").substr(17)));
    }
    auto http = makeCurlHttpClient(1);
    auto f = http->request(Method::Get, "http://127.0.0.1:" + std::to_string(port) + "/", "");
    ASSERT_EQ(f.wait_for(5s), std::future_status::ready);
    EXPECT_THROW(f.get(), std::runtime_error);
}

TEST(CurlHttp, DestroyingClientWithQueuedRequestsDoesNotHang) {
    auto http = makeCurlHttpClient(1);
    std::future<HttpResponse> f =
        http->request(Method::Get, "http://127.0.0.1:1/", "");  // refused immediately
    http.reset();
    // either completed or dropped; the test passes if reset() returned
    SUCCEED();
}

TEST(CurlHttp, DestroyingTheClientAbortsTransfersInFlightPromptly) {
    cardinal::testing::RoutedServer slow([](const cardinal::testing::HttpRequestInfo&) {
        std::this_thread::sleep_for(3s);
        return cardinal::testing::HttpReply{200, "{}"};
    });
    std::future<HttpResponse> f;
    auto start = std::chrono::steady_clock::now();
    {
        auto http = makeCurlHttpClient(1);
        f = http->request(Method::Get, slow.url() + "/slow", "");
        for (int i = 0; i < 200 && slow.requests().empty(); ++i) std::this_thread::sleep_for(5ms);
        ASSERT_FALSE(slow.requests().empty());  // the transfer is really in flight
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms);
    ASSERT_EQ(f.wait_for(0ms), std::future_status::ready);
    EXPECT_THROW(f.get(), std::runtime_error);
}

TEST(CurlHttp, RequestsStillQueuedWhenShuttingDownFailInsteadOfRunning) {
    cardinal::testing::RoutedServer slow([](const cardinal::testing::HttpRequestInfo&) {
        std::this_thread::sleep_for(2s);
        return cardinal::testing::HttpReply{200, "{}"};
    });
    std::future<HttpResponse> running, queued;
    auto start = std::chrono::steady_clock::now();
    {
        auto http = makeCurlHttpClient(1);  // one worker: the second request waits in the queue
        running = http->request(Method::Get, slow.url() + "/a", "");
        queued = http->request(Method::Get, slow.url() + "/b", "");
        for (int i = 0; i < 200 && slow.requests().empty(); ++i) std::this_thread::sleep_for(5ms);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms);
    EXPECT_THROW(running.get(), std::runtime_error);
    EXPECT_THROW(queued.get(), std::runtime_error);
    EXPECT_EQ(slow.count("GET", "/b"), 0u);  // never sent
}

TEST(CurlHttp, ShutdownIsIdempotentAndLaterRequestsFail) {
    auto http = makeCurlHttpClient(1);
    http->shutdown();
    http->shutdown();
    auto f = http->request(Method::Get, "http://127.0.0.1:1/", "");
    ASSERT_EQ(f.wait_for(2s), std::future_status::ready);
    EXPECT_THROW(f.get(), std::runtime_error);
}
