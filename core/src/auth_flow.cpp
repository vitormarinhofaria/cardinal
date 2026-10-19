#include "cardinal/auth_flow.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <random>
#include <stdexcept>

#include "cardinal/trello_api.h"  // urlEncode

namespace cardinal {
namespace {

using namespace std::chrono_literals;

constexpr size_t kMaxRequest = 16 * 1024;

const char* kCallbackPage = R"html(<!doctype html>
<meta charset="utf-8">
<title>Cardinal</title>
<body style="font-family:sans-serif;max-width:32em;margin:4em auto">
<p id="m">Conectando ao Cardinal…</p>
<script>
const token = new URLSearchParams(location.hash.slice(1)).get('token');
const nonce = new URLSearchParams(location.search).get('nonce');
const say = t => { document.getElementById('m').textContent = t; };
if (!token) {
  say('O Trello não enviou um token. Feche esta janela e tente de novo no Cardinal.');
} else {
  fetch('/token?nonce=' + encodeURIComponent(nonce), {
    method: 'POST', body: token, headers: {'Content-Type': 'text/plain'}
  }).then(r => say(r.ok ? 'Pronto! Pode fechar esta janela e voltar ao Cardinal.'
                        : 'Não foi possível entregar o token ao Cardinal.'))
    .catch(() => say('Não foi possível entregar o token ao Cardinal.'));
  history.replaceState(null, '', location.pathname);  // drop the token from the URL
}
</script>
)html";

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

bool looksLikeToken(const std::string& t) {
    if (t.size() < 16 || t.size() > 512) return false;
    return std::all_of(t.begin(), t.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

struct Request {
    std::string method, path, query, host, body;
    bool valid = false;
};

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

void respond(int fd, int status, const char* reason, const std::string& body,
             const char* type = "text/plain; charset=utf-8") {
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

void finish(const std::shared_ptr<std::promise<std::string>>& p, std::exception_ptr e) {
    try {
        p->set_exception(std::move(e));
    } catch (const std::future_error&) {
    }
}

}  // namespace

std::string manualAuthorizeUrl(const std::string& apiKey, const std::string& appName) {
    return "https://trello.com/1/authorize?key=" + urlEncode(apiKey) + "&name=" + urlEncode(appName) +
           "&expiration=never&response_type=token&scope=" + urlEncode("read,write");
}

AuthFlow::AuthFlow(std::string apiKey, BrowserOpener& opener, AuthOptions options)
    : apiKey_(std::move(apiKey)), opener_(opener), options_(std::move(options)) {}

AuthFlow::~AuthFlow() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

std::future<std::string> AuthFlow::run() {
    if (started_) throw std::logic_error("AuthFlow::run called twice");
    started_ = true;
    auto promise = std::make_shared<std::promise<std::string>>();
    auto future = promise->get_future();

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never reachable from the network
    addr.sin_port = htons(options_.port);
    socklen_t len = sizeof addr;
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(fd, 4) != 0 || ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        if (fd >= 0) ::close(fd);
        finish(promise, std::make_exception_ptr(AuthError(
                            AuthError::Kind::ListenFailed,
                            "could not listen on 127.0.0.1:" + std::to_string(options_.port))));
        return future;
    }
    const unsigned short port = ntohs(addr.sin_port);
    const std::string nonce = randomHex(16);
    thread_ = std::thread([this, fd, port, nonce, promise] { serve(fd, port, nonce, promise); });

    const std::string returnUrl =
        "http://127.0.0.1:" + std::to_string(port) + "/callback?nonce=" + nonce;
    const std::string url = options_.authorizeUrl + "?key=" + urlEncode(apiKey_) +
                            "&name=" + urlEncode(options_.appName) + "&expiration=never" +
                            "&response_type=token&scope=" + urlEncode("read,write") +
                            "&callback_method=fragment&return_url=" + urlEncode(returnUrl);
    try {
        opener_.open(url);
    } catch (const std::exception& e) {
        finish(promise, std::make_exception_ptr(AuthError(
                            AuthError::Kind::BrowserFailed,
                            std::string("could not open the browser: ") + e.what())));
        stop_ = true;
    }
    return future;
}

void AuthFlow::serve(int listenFd, unsigned short port, std::string nonce,
                     std::shared_ptr<std::promise<std::string>> promise) {
    const auto deadline = std::chrono::steady_clock::now() + options_.timeout;
    const std::string hostA = "127.0.0.1:" + std::to_string(port);
    const std::string hostB = "localhost:" + std::to_string(port);
    std::optional<std::string> token;

    while (!token && !stop_) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left <= 0ms) {
            finish(promise, std::make_exception_ptr(
                                AuthError(AuthError::Kind::TimedOut, "login timed out")));
            break;
        }
        pollfd pfd{listenFd, POLLIN, 0};
        if (::poll(&pfd, 1, static_cast<int>(std::min(left, 100ms).count())) <= 0) continue;
        int client = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;

        Request req = readRequest(client);
        if (!req.valid) {
            respond(client, 400, "Bad Request", "bad request");
        } else if (req.host != hostA && req.host != hostB) {
            respond(client, 403, "Forbidden", "forbidden");  // DNS rebinding guard
        } else if (req.path == "/callback") {
            if (req.method != "GET")
                respond(client, 405, "Method Not Allowed", "method not allowed");
            else if (!sameSecret(queryValue(req.query, "nonce"), nonce))
                respond(client, 403, "Forbidden", "forbidden");
            else
                respond(client, 200, "OK", kCallbackPage, "text/html; charset=utf-8");
        } else if (req.path == "/token") {
            if (req.method != "POST")
                respond(client, 405, "Method Not Allowed", "method not allowed");
            else if (!sameSecret(queryValue(req.query, "nonce"), nonce))
                respond(client, 403, "Forbidden", "forbidden");
            else if (!looksLikeToken(req.body))
                respond(client, 400, "Bad Request", "bad token");
            else {
                respond(client, 200, "OK", "ok");
                token = req.body;
            }
        } else {
            respond(client, 404, "Not Found", "not found");
        }
        ::close(client);
    }
    ::close(listenFd);
    if (token) {
        try {
            promise->set_value(*token);
        } catch (const std::future_error&) {  // already failed (e.g. browser could not open)
        }
    } else {
        finish(promise, std::make_exception_ptr(
                                AuthError(AuthError::Kind::Cancelled, "login cancelled")));
    }
}

}  // namespace cardinal
