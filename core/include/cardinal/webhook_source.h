#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "cardinal/debouncer.h"
#include "cardinal/event_source.h"
#include "cardinal/trello_api.h"
#include "cardinal/tunnel.h"

namespace cardinal {

struct WebhookOptions {
    TrelloApi* api = nullptr;          // required; outlives the source
    std::filesystem::path dataDir;     // holds install-id
    // Builds the tunnel (downloading cloudflared if needed); runs on the source's thread. Throwing
    // CloudflaredUnavailable turns webhooks off for the session; anything else is retried.
    std::function<std::unique_ptr<Tunnel>()> makeTunnel;

    // Same contract as BoardSocketOptions: called on the source's threads.
    std::function<void(const std::string& boardId)> onActivity;
    std::function<void(bool connected)> onConnected;
    std::function<void(const std::string&)> onLog;

    std::chrono::milliseconds quiet{1000};
    std::chrono::milliseconds maxDelay{3000};
    std::chrono::milliseconds tick{200};
    std::chrono::milliseconds minBackoff{1000};
    std::chrono::milliseconds maxBackoff{60000};
    // Trello checks the callback right away, before the new tunnel's DNS name may resolve.
    std::chrono::milliseconds validationRetry{3000};
    std::chrono::milliseconds validationWindow{60000};
    std::chrono::milliseconds deleteTimeout{2000};
    bool autoStart = true;
};

// Trello webhooks used only as a trigger: any delivery for the watched board means "reload it";
// the payload is not read. A local HTTP server on 127.0.0.1 is reached through a tunnel, and one
// webhook per installation (described "Cardinal <install-id>") is pointed at the tunnel's URL and
// at the board being watched; it is deleted on stop(). Best effort: on failure it retries with
// backoff while polling carries on.
class WebhookEventSource : public EventSource {
public:
    explicit WebhookEventSource(WebhookOptions options);
    ~WebhookEventSource() override;
    WebhookEventSource(const WebhookEventSource&) = delete;
    WebhookEventSource& operator=(const WebhookEventSource&) = delete;

    void watch(const std::string& boardId, Handler handler) override;
    void unwatch(const std::string& boardId) override;

    void start();
    void stop();  // deletes the webhook (bounded wait), closes the tunnel, joins the threads

    unsigned short port() const { return port_; }
    const std::string& path() const { return path_; }  // the secret callback path

private:
    struct GiveUp : std::runtime_error {
        using std::runtime_error::runtime_error;
    };
    struct Stopped {};

    void serve();
    void run();
    void cleanup();
    void ensureWebhook(const std::string& callback, const std::string& board);
    template <class T>
    T await(std::future<T> f);
    template <class F>
    auto withValidationRetry(F call) -> decltype(call());
    std::string wanted();
    bool stopping();
    void sleepFor(std::chrono::milliseconds d);
    void log(const std::string& line);
    std::string installId();

    WebhookOptions options_;
    int listenFd_ = -1;
    unsigned short port_ = 0;
    std::string path_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::string board_;
    bool stop_ = false;
    Debouncer pending_;  // deliveries not yet reported; under mutex_

    // Manager thread only.
    std::unique_ptr<Tunnel> tunnel_;
    std::string webhookId_;

    std::thread server_, manager_;
};

}  // namespace cardinal
