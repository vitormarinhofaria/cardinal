#pragma once

// A small multi-request HTTP/1.1 server on 127.0.0.1 for integration tests. One connection per
// request ("Connection: close"), served one at a time on a background thread.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cardinal::testing {

struct HttpRequestInfo {
    std::string method;
    std::string path;   // without query
    std::string query;  // raw, still percent-encoded
    std::string body;
};

struct HttpReply {
    int status = 200;
    std::string body = "{}";
};

class RoutedServer {
public:
    using Handler = std::function<HttpReply(const HttpRequestInfo&)>;

    explicit RoutedServer(Handler handler) : handler_(std::move(handler)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        socklen_t len = sizeof addr;
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::listen(fd_, 16);
        thread_ = std::thread([this] { serve(); });
    }
    ~RoutedServer() {
        stop_ = true;
        thread_.join();
        ::close(fd_);
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    std::vector<HttpRequestInfo> requests() {
        std::lock_guard lk(mu_);
        return log_;
    }
    size_t count(const std::string& method, const std::string& path) {
        size_t n = 0;
        for (const auto& r : requests())
            if (r.method == method && r.path == path) ++n;
        return n;
    }

private:
    void serve() {
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue;
            int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) continue;
            handle(c);
            ::close(c);
        }
    }

    void handle(int c) {
        timeval tv{2, 0};
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        std::string raw;
        char buf[4096];
        size_t headerEnd = std::string::npos, want = 0;
        while (true) {
            ssize_t n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            raw.append(buf, static_cast<size_t>(n));
            if (headerEnd == std::string::npos) {
                headerEnd = raw.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    std::string head = raw.substr(0, headerEnd);
                    for (auto& ch : head) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    auto cl = head.find("content-length: ");
                    want = headerEnd + 4 + (cl == std::string::npos ? 0 : std::stoul(head.substr(cl + 16)));
                }
            }
            if (headerEnd != std::string::npos && raw.size() >= want) break;
        }
        if (headerEnd == std::string::npos) return;
        HttpRequestInfo req;
        auto sp1 = raw.find(' ');
        auto sp2 = raw.find(' ', sp1 + 1);
        req.method = raw.substr(0, sp1);
        std::string target = raw.substr(sp1 + 1, sp2 - sp1 - 1);
        auto q = target.find('?');
        req.path = target.substr(0, q);
        if (q != std::string::npos) req.query = target.substr(q + 1);
        req.body = raw.substr(headerEnd + 4);
        {
            std::lock_guard lk(mu_);
            log_.push_back(req);
        }
        HttpReply r = handler_(req);
        std::string out = "HTTP/1.1 " + std::to_string(r.status) + " X\r\nContent-Type: application/json\r\n"
                          "Content-Length: " + std::to_string(r.body.size()) +
                          "\r\nConnection: close\r\n\r\n" + r.body;
        ::send(c, out.data(), out.size(), MSG_NOSIGNAL);
    }

    Handler handler_;
    int fd_ = -1;
    unsigned short port_ = 0;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::vector<HttpRequestInfo> log_;
    std::thread thread_;
};

}  // namespace cardinal::testing
