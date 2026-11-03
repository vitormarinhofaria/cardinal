#include <curl/curl.h>
#include <poll.h>

#include <mutex>
#include <stdexcept>

#include "cardinal/websocket.h"

namespace cardinal {
namespace {

void initCurlOnce() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

[[noreturn]] void fail(const std::string& what, CURLcode rc) {
    throw std::runtime_error("websocket: " + what + ": " + curl_easy_strerror(rc));
}

class CurlWebSocket : public WebSocketConnection {
public:
    explicit CurlWebSocket(CURL* curl, curl_slist* headers) : curl_(curl), headers_(headers) {}
    ~CurlWebSocket() override {
        if (curl_) {
            size_t sent = 0;
            curl_ws_send(curl_, "", 0, &sent, 0, CURLWS_CLOSE);  // polite; ignore failures
            curl_easy_cleanup(curl_);
        }
        curl_slist_free_all(headers_);
    }

    void send(const std::string& text) override {
        size_t offset = 0;
        while (offset < text.size()) {
            size_t sent = 0;
            CURLcode rc = curl_ws_send(curl_, text.data() + offset, text.size() - offset, &sent, 0,
                                       CURLWS_TEXT);
            offset += sent;
            if (rc == CURLE_AGAIN) {
                wait(POLLOUT, std::chrono::seconds(10));
                continue;
            }
            if (rc != CURLE_OK) fail("send", rc);
        }
    }

    std::optional<std::string> receive(std::chrono::milliseconds timeout) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        char buffer[16384];
        for (;;) {
            size_t got = 0;
            const curl_ws_frame* meta = nullptr;
            CURLcode rc = curl_ws_recv(curl_, buffer, sizeof buffer, &got, &meta);
            if (rc == CURLE_AGAIN) {
                // Mid-message we keep waiting past the deadline: the rest is already on its way.
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0 && pending_.empty()) return std::nullopt;
                if (left.count() <= 0) left = std::chrono::milliseconds(1000);
                if (!wait(POLLIN, left) && pending_.empty()) return std::nullopt;
                continue;
            }
            if (rc != CURLE_OK) fail("receive", rc);
            if (!meta) continue;
            if (meta->flags & CURLWS_CLOSE) throw std::runtime_error("websocket: closed by server");
            if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue;  // libcurl answers pings
            pending_.append(buffer, got);
            const bool frameDone = meta->bytesleft == 0;
            const bool lastFragment = !(meta->flags & CURLWS_CONT);
            if (frameDone && lastFragment) {
                std::string message;
                message.swap(pending_);
                return message;
            }
        }
    }

private:
    // true when the socket became ready in time.
    bool wait(short events, std::chrono::milliseconds timeout) {
        curl_socket_t fd = CURL_SOCKET_BAD;
        if (curl_easy_getinfo(curl_, CURLINFO_ACTIVESOCKET, &fd) != CURLE_OK || fd == CURL_SOCKET_BAD)
            throw std::runtime_error("websocket: connection lost");
        pollfd p{fd, events, 0};
        int n = ::poll(&p, 1, static_cast<int>(timeout.count()));
        if (n < 0) throw std::runtime_error("websocket: poll failed");
        if (n > 0 && (p.revents & (POLLERR | POLLNVAL)))
            throw std::runtime_error("websocket: connection lost");
        return n > 0;
    }

    CURL* curl_;
    curl_slist* headers_;
    std::string pending_;
};

class CurlWebSocketConnector : public WebSocketConnector {
public:
    std::unique_ptr<WebSocketConnection> connect(const std::string& url,
                                                 const std::vector<std::string>& headers,
                                                 std::function<bool()> cancelled) override {
        initCurlOnce();
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("websocket: curl_easy_init failed");
        curl_slist* list = nullptr;
        for (const auto& h : headers) list = curl_slist_append(list, h.c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);  // websocket: upgrade, then hand over
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);  // the handshake only
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Cardinal");
        if (list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
        if (cancelled) {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelled);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                             +[](void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                                 return (*static_cast<std::function<bool()>*>(data))() ? 1 : 0;
                             });
        }
        CURLcode rc = curl_easy_perform(curl);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);  // `cancelled` dies with this call
        if (rc != CURLE_OK) {
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            curl_easy_cleanup(curl);
            curl_slist_free_all(list);
            std::string what = "connect";
            if (status) what += " (HTTP " + std::to_string(status) + ")";
            fail(what, rc);
        }
        return std::make_unique<CurlWebSocket>(curl, list);
    }
};

}  // namespace

std::unique_ptr<WebSocketConnector> makeCurlWebSocketConnector() {
    return std::make_unique<CurlWebSocketConnector>();
}

}  // namespace cardinal
