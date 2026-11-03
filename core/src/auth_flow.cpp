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
#include "loopback_http.h"

namespace cardinal {
namespace {

using namespace std::chrono_literals;

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

bool looksLikeToken(const std::string& t) {
    if (t.size() < 16 || t.size() > 512) return false;
    return std::all_of(t.begin(), t.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
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

    const auto listener = listenLoopback(options_.port);
    const int fd = listener.fd;
    const unsigned short port = listener.port;
    if (fd < 0) {
        finish(promise, std::make_exception_ptr(AuthError(
                            AuthError::Kind::ListenFailed,
                            "could not listen on 127.0.0.1:" + std::to_string(options_.port))));
        return future;
    }
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
