#pragma once

// A minimal HTTP/1.1 server on 127.0.0.1, one request per connection. Internal to the core: used
// by the browser login (AuthFlow) and the webhook receiver.

#include <cstddef>
#include <string>

namespace cardinal {

constexpr size_t kMaxRequest = 16 * 1024;  // headers + body; anything longer is cut

struct Request {
    std::string method, path, query, host, body;
    bool valid = false;
};

struct Listener {
    int fd = -1;  // -1: could not listen
    unsigned short port = 0;
};

// Listens on 127.0.0.1:`port` (0 = any free port). The caller closes the fd.
Listener listenLoopback(unsigned short port);

Request readRequest(int fd);
std::string queryValue(const std::string& query, const std::string& key);
// Writes a whole response and leaves the connection for the caller to close.
void respond(int fd, int status, const char* reason, const std::string& body,
             const char* type = "text/plain; charset=utf-8");

std::string randomHex(size_t bytes);
bool sameSecret(const std::string& a, const std::string& b);  // constant time

}  // namespace cardinal
