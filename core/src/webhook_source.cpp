#include "cardinal/webhook_source.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>
#include <type_traits>

#include "cardinal/cloudflared.h"
#include "loopback_http.h"

namespace cardinal {

using SteadyClock = std::chrono::steady_clock;

WebhookEventSource::WebhookEventSource(WebhookOptions options)
    : options_(std::move(options)), pending_(options_.quiet, options_.maxDelay) {
    const Listener l = listenLoopback(0);
    if (l.fd < 0) throw std::runtime_error("webhook: cannot listen on 127.0.0.1");
    listenFd_ = l.fd;
    port_ = l.port;
    path_ = "/hook/" + randomHex(16);
    if (options_.autoStart) start();
}

WebhookEventSource::~WebhookEventSource() {
    stop();
    if (listenFd_ >= 0) ::close(listenFd_);
}

void WebhookEventSource::start() {
    if (server_.joinable()) return;
    server_ = std::thread([this] { serve(); });
    manager_ = std::thread([this] { run(); });
}

void WebhookEventSource::stop() {
    {
        std::lock_guard lk(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (manager_.joinable()) manager_.join();  // it cleans up on its way out
    if (server_.joinable()) server_.join();
}

void WebhookEventSource::watch(const std::string& boardId, Handler) {
    {
        std::lock_guard lk(mutex_);
        board_ = boardId;
    }
    wake_.notify_all();
}

void WebhookEventSource::unwatch(const std::string& boardId) {
    std::lock_guard lk(mutex_);
    if (board_ == boardId) board_.clear();
}

std::string WebhookEventSource::wanted() {
    std::lock_guard lk(mutex_);
    return board_;
}

bool WebhookEventSource::stopping() {
    std::lock_guard lk(mutex_);
    return stop_;
}

void WebhookEventSource::sleepFor(std::chrono::milliseconds d) {
    std::unique_lock lk(mutex_);
    if (wake_.wait_for(lk, d, [this] { return stop_; })) throw Stopped{};
}

void WebhookEventSource::log(const std::string& line) {
    if (options_.onLog) options_.onLog(line);
}

// ---- the HTTP side: deliveries only mark the board dirty ----

void WebhookEventSource::serve() {
    while (!stopping()) {
        pollfd p{listenFd_, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(options_.tick.count())) <= 0) continue;
        int client = ::accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        Request req = readRequest(client);
        if (!req.valid) {
            respond(client, 400, "Bad Request", "");
        } else if (!sameSecret(req.path, path_)) {
            respond(client, 404, "Not Found", "");
        } else if (req.method == "HEAD" || req.method == "GET") {
            respond(client, 200, "OK", "");  // Trello checking the callback
        } else if (req.method == "POST") {
            respond(client, 200, "OK", "");
            log("delivery received");
            {
                std::lock_guard lk(mutex_);
                pending_.note(SteadyClock::now());
            }
            wake_.notify_all();
        } else {
            respond(client, 405, "Method Not Allowed", "");
        }
        ::close(client);
    }
}

// ---- the management side: tunnel + webhook ----

// The API's futures are deferred and may wait on the network for long: wait on another thread so
// stop() is never held up by a slow request.
template <class T>
T WebhookEventSource::await(std::future<T> f) {
    auto box = std::make_shared<std::promise<T>>();
    auto result = box->get_future();
    std::thread([f = std::move(f), box]() mutable {
        try {
            if constexpr (std::is_void_v<T>) {
                f.get();
                box->set_value();
            } else {
                box->set_value(f.get());
            }
        } catch (...) {
            box->set_exception(std::current_exception());
        }
    }).detach();
    while (result.wait_for(options_.tick) != std::future_status::ready)
        if (stopping()) throw Stopped{};
    return result.get();
}

template <class F>
auto WebhookEventSource::withValidationRetry(F call) -> decltype(call()) {
    const auto deadline = SteadyClock::now() + options_.validationWindow;
    for (;;) {
        try {
            return call();
        } catch (const ApiError& e) {
            if (e.status() == 401 || e.status() == 403)
                throw GiveUp("Trello refused the webhook (HTTP " + std::to_string(e.status()) + "): " + e.body());
            if (e.status() != 400 || SteadyClock::now() >= deadline) throw;
            log("Trello could not reach the callback yet (" + e.body() + "), retrying");
            sleepFor(options_.validationRetry);
        }
    }
}

std::string WebhookEventSource::installId() {
    const auto file = options_.dataDir / "install-id";
    std::string id;
    {
        std::ifstream in(file);
        std::getline(in, id);
    }
    if (id.size() == 16) return id;
    id = randomHex(8);
    std::filesystem::create_directories(options_.dataDir);
    std::ofstream(file) << id << "\n";
    return id;
}

void WebhookEventSource::ensureWebhook(const std::string& callback, const std::string& board) {
    TrelloApi& api = *options_.api;
    const std::string description = "Cardinal " + installId();
    if (webhookId_.empty()) {
        for (const auto& w : withValidationRetry([&] { return await(api.webhooks()); })) {
            if (w.description != description) continue;  // another machine's, or not ours
            if (webhookId_.empty()) {
                webhookId_ = w.id;
            } else {
                try {
                    await(api.deleteWebhook(w.id));  // a leftover duplicate
                } catch (const ApiError&) {
                }
            }
        }
    }
    if (!webhookId_.empty()) {
        try {
            withValidationRetry([&] { return await(api.updateWebhook(webhookId_, callback, board)); });
            log("webhook " + webhookId_ + " now points at board " + board);
            return;
        } catch (const ApiError& e) {
            if (e.status() != 404) throw;
            webhookId_.clear();  // deleted meanwhile: make a new one
        }
    }
    auto created = withValidationRetry(
        [&] { return await(api.createWebhook(description, callback, board)); });
    webhookId_ = created.id;
    log("webhook " + webhookId_ + " created for board " + board);
}

void WebhookEventSource::run() {
    auto backoff = options_.minBackoff;
    bool everHealthy = false;
    bool healthy = false;
    auto setHealthy = [&](bool h) {
        if (h == healthy) return;
        healthy = h;
        if (options_.onConnected) options_.onConnected(h);
    };
    try {
        for (;;) {
            {
                std::unique_lock lk(mutex_);
                wake_.wait(lk, [this] { return stop_ || !board_.empty(); });
                if (stop_) break;
            }
            try {
                if (!tunnel_) tunnel_ = options_.makeTunnel();
                const std::string base = tunnel_->open(port_, [this] { return stopping(); });
                const std::string callback = base + path_;
                log("tunnel up at " + base);
                std::string registered = wanted();
                if (registered.empty()) continue;
                ensureWebhook(callback, registered);
                {
                    std::lock_guard lk(mutex_);
                    pending_.reset();
                }
                setHealthy(true);
                backoff = options_.minBackoff;
                if (everHealthy && options_.onActivity) {
                    log("reconnected: reloading " + registered);
                    options_.onActivity(registered);  // deliveries may have been lost meanwhile
                }
                everHealthy = true;

                for (;;) {
                    std::string fire;
                    {
                        std::unique_lock lk(mutex_);
                        wake_.wait_for(lk, options_.tick);
                        if (stop_) throw Stopped{};
                        if (pending_.due(SteadyClock::now())) {
                            pending_.reset();
                            if (board_ == registered) fire = registered;
                        }
                    }
                    if (!fire.empty()) {
                        log("board changed: reloading " + fire);
                        if (options_.onActivity) options_.onActivity(fire);
                    }
                    if (!tunnel_->alive()) throw std::runtime_error("tunnel closed");
                    const std::string want = wanted();
                    if (!want.empty() && want != registered) {
                        ensureWebhook(callback, want);
                        registered = want;
                        std::lock_guard lk(mutex_);
                        pending_.reset();  // deliveries for the previous board
                    }
                }
            } catch (const GiveUp& e) {
                log(std::string(e.what()) + ": webhooks off, polling only");
                setHealthy(false);
                std::unique_lock lk(mutex_);
                wake_.wait(lk, [this] { return stop_; });
                break;
            } catch (const CloudflaredUnavailable& e) {
                log(std::string(e.what()) + ": webhooks off, polling only");
                setHealthy(false);
                std::unique_lock lk(mutex_);
                wake_.wait(lk, [this] { return stop_; });
                break;
            } catch (const std::exception& e) {
                log(std::string("webhook link lost: ") + e.what());
                setHealthy(false);
            }
            sleepFor(backoff);
            backoff = std::min(backoff * 2, options_.maxBackoff);
        }
    } catch (const Stopped&) {
    }
    cleanup();
}

void WebhookEventSource::cleanup() {
    if (!webhookId_.empty()) {
        // Best effort, bounded: a webhook left behind is reused next time (same install-id).
        auto done = std::make_shared<std::promise<void>>();
        auto finished = done->get_future();
        try {
            std::thread([f = options_.api->deleteWebhook(webhookId_), done]() mutable {
                try {
                    f.get();
                } catch (...) {
                }
                done->set_value();
            }).detach();
            if (finished.wait_for(options_.deleteTimeout) == std::future_status::ready)
                log("webhook " + webhookId_ + " deleted");
            else
                log("webhook " + webhookId_ + " not deleted in time; reused next start");
        } catch (const std::exception& e) {
            log(std::string("webhook delete failed: ") + e.what());
        }
        webhookId_.clear();
    }
    if (tunnel_) tunnel_->close();
}

}  // namespace cardinal
