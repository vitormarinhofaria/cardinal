#include "loopback_http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <random>

namespace cardinal {

Listener listenLoopback(unsigned short port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never reachable from the network
    addr.sin_port = htons(port);
    socklen_t len = sizeof addr;
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(fd, 8) != 0 || ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        if (fd >= 0) ::close(fd);
        return {};
    }
    return {fd, ntohs(addr.sin_port)};
}

std::string randomHex(size_t bytes) {
    std::random_device rd;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < bytes; ++i) {
        unsigned v = rd() & 0xff;
        out += kHex[v >> 4];
        out += kHex[v & 15];
    }
    return out;
}

bool sameSecret(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

Request readRequest(int fd) {
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string raw;
    char buf[2048];
    size_t headerEnd = std::string::npos, want = 0;
    while (raw.size() < kMaxRequest) {
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        raw.append(buf, static_cast<size_t>(n));
        if (headerEnd == std::string::npos) {
            headerEnd = raw.find("\r\n\r\n");
            if (headerEnd == std::string::npos) continue;
            std::string head = raw.substr(0, headerEnd);
            std::transform(head.begin(), head.end(), head.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            auto cl = head.find("\r\ncontent-length:");
            size_t len = 0;
            if (cl != std::string::npos) len = std::strtoul(head.c_str() + cl + 17, nullptr, 10);
            want = headerEnd + 4 + std::min(len, kMaxRequest);
        }
        if (raw.size() >= want) break;
    }
    Request r;
    if (headerEnd == std::string::npos) return r;

    auto lineEnd = raw.find("\r\n");
    std::string line = raw.substr(0, lineEnd);
    auto sp1 = line.find(' ');
    auto sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1) return r;
    r.method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    auto q = target.find('?');
    r.path = target.substr(0, q);
    if (q != std::string::npos) r.query = target.substr(q + 1);

    std::string head = raw.substr(lineEnd, headerEnd - lineEnd);
    std::string lower = head;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto h = lower.find("\r\nhost:");
    if (h != std::string::npos) {
        auto end = head.find("\r\n", h + 2);
        r.host = head.substr(h + 7, end == std::string::npos ? std::string::npos : end - h - 7);
        r.host.erase(0, r.host.find_first_not_of(" \t"));
        r.host.erase(r.host.find_last_not_of(" \t") + 1);
    }
    r.body = raw.substr(headerEnd + 4);
    r.valid = true;
    return r;
}

std::string queryValue(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos <= query.size()) {
        auto amp = query.find('&', pos);
        std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (pair.rfind(key + "=", 0) == 0) return pair.substr(key.size() + 1);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

void respond(int fd, int status, const char* reason, const std::string& body, const char* type) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                      "\r\nContent-Type: " + type +
                      "\r\nContent-Length: " + std::to_string(body.size()) +
                      "\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer"
                      "\r\nContent-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; "
                      "connect-src 'self'; style-src 'unsafe-inline'"
                      "\r\nConnection: close\r\n\r\n" +
                      body;
    ::send(fd, out.data(), out.size(), MSG_NOSIGNAL);
}

}  // namespace cardinal
