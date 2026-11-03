#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "cardinal/board_socket.h"
#include "cardinal/event_source.h"
#include "cardinal/webhook_source.h"
#include "cardinal/executor.h"
#include "cardinal/http.h"
#include "cardinal/image_cache.h"
#include "cardinal/scheduler.h"
#include "cardinal/store.h"
#include "cardinal/sync.h"
#include "cardinal/trello_api.h"
#include "cardinal/viewmodel.h"
#include "cardinal/write_queue.h"

namespace cardinal {

struct AppConfig {
    std::filesystem::path dataDir;  // holds cache.db; created if missing
    std::string apiKey;
    std::string token;
    std::string apiBase = "https://api.trello.com/1";
    std::chrono::milliseconds pollInterval{20000};
    // Live "board changed" triggers; while one works, polling is only a safety net.
    // Webhooks through a cloudflared quick tunnel (cloudflared is downloaded into dataDir/bin).
    bool webhook = true;
    bool webhookDirect = false;  // no tunnel: Trello calls 127.0.0.1 itself (a local mock only)
    // Trello's websocket; it does not accept API tokens, so it is off unless a URL is given.
    std::string socketUrl;
    std::chrono::milliseconds pollIntervalWithSocket{120000};
    std::function<void(const std::string&)> liveLog;  // diagnostics, optional
    int workerThreads = 4;
    RateLimit rate;
    SyncOptions sync;
    WriteOptions write;
};

// The composition root: wires cache, network, scheduling, sync, writes and view-models. A UI only
// supplies a UiDispatcher. Destroy every view-model obtained from model() before this object.
class AppContext {
public:
    AppContext(AppConfig config, UiDispatcher& ui);
    ~AppContext();
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    AppModel& model() { return *model_; }
    Store& store() { return *store_; }
    ImageCache& images() { return *images_; }

private:
    // Declaration order is destruction order in reverse: model first, store last.
    std::unique_ptr<Store> store_;
    std::unique_ptr<HttpClient> http_;
    std::unique_ptr<HttpClient> imageHttp_;  // own transfers: big downloads never delay the API
    std::unique_ptr<ImageCache> images_;
    SystemClock clock_;
    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<TrelloApi> api_;
    std::unique_ptr<ThreadPoolExecutor> pool_;
    std::unique_ptr<PollingEventSource> poller_;
    std::unique_ptr<WebSocketConnector> wsConnector_;
    std::unique_ptr<BoardSocket> socket_;               // null when disabled
    std::unique_ptr<WebhookEventSource> webhook_;       // null when disabled
    std::atomic<int> liveSources_{0};                   // triggers currently working
    std::unique_ptr<CompositeEventSource> events_;
    std::unique_ptr<AppModel> model_;
    std::atomic<AppModel*> modelRef_{nullptr};  // for the poll thread's callbacks
};

}  // namespace cardinal
