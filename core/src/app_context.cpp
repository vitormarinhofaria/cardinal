#include "cardinal/app_context.h"

#include <algorithm>

#include "cardinal/cloudflared.h"

namespace cardinal {

AppContext::AppContext(AppConfig config, UiDispatcher& ui) {
    std::filesystem::create_directories(config.dataDir);
    store_ = std::make_unique<Store>(Store::open(config.dataDir / "cache.db"));
    http_ = makeCurlHttpClient(config.workerThreads);
    imageHttp_ = makeCurlHttpClient(3);
    ImageCacheOptions imageOptions;
    imageOptions.dir = config.dataDir / "images";
    imageOptions.apiKey = config.apiKey;
    imageOptions.token = config.token;
    // Uploaded attachments live on trello.com; a test or mock server gets the token too.
    const std::string apiOrigin = ImageCache::originOf(config.apiBase);
    if (!apiOrigin.empty()) imageOptions.authOrigins.push_back(apiOrigin);
    images_ = std::make_unique<ImageCache>(*imageHttp_, std::move(imageOptions));
    scheduler_ = std::make_unique<Scheduler>(*http_, config.rate, clock_);
    api_ = std::make_unique<TrelloApi>(*scheduler_, config.apiKey, config.token, config.apiBase);
    pool_ = std::make_unique<ThreadPoolExecutor>(config.workerThreads);

    PollingOptions polling;
    polling.interval = config.pollInterval;
    polling.onSuccess = [this] {
        if (auto* m = modelRef_.load()) m->onNetworkOk();
    };
    polling.onError = [this](const std::exception& e) {
        if (auto* m = modelRef_.load()) m->onNetworkError(describe(e));
    };
    poller_ = std::make_unique<PollingEventSource>(*api_, *store_, polling);

    std::vector<EventSource*> sources{poller_.get()};
    auto onActivity = [this](const std::string& boardId) {
        if (auto* m = modelRef_.load()) m->refreshBoard(boardId);
    };
    auto onConnected = [this, slow = std::max(config.pollIntervalWithSocket, config.pollInterval),
                        normal = config.pollInterval](bool connected) {
        const int live = connected ? ++liveSources_ : --liveSources_;
        poller_->setInterval(live > 0 ? slow : normal);
    };
    auto logAs = [log = config.liveLog](const char* who) -> std::function<void(const std::string&)> {
        if (!log) return {};
        return [log, who](const std::string& line) { log(std::string("[") + who + "] " + line); };
    };
    if (config.webhook) {
        WebhookOptions hook;
        hook.api = api_.get();
        hook.dataDir = config.dataDir;
        hook.onActivity = onActivity;
        hook.onConnected = onConnected;
        hook.onLog = logAs("webhook");
        if (config.webhookDirect) {
            hook.makeTunnel = [] { return std::make_unique<DirectTunnel>(); };
        } else {
            hook.makeTunnel = [this, dir = config.dataDir, log = hook.onLog]() -> std::unique_ptr<Tunnel> {
                if (log) log("checking cloudflared " + std::string(kCloudflaredVersion));
                auto bin = ensureCloudflared(*imageHttp_, dir / "bin");  // big download: own client
                if (log) log("using " + bin.string());
                return std::make_unique<CloudflaredTunnel>(bin, dir / "cloudflared-home", log);
            };
        }
        try {
            webhook_ = std::make_unique<WebhookEventSource>(std::move(hook));
            sources.push_back(webhook_.get());
        } catch (const std::exception& e) {  // no local port: polling only
            if (config.liveLog) config.liveLog(std::string("[webhook] ") + e.what());
        }
    }
    if (!config.socketUrl.empty()) {
        wsConnector_ = makeCurlWebSocketConnector();
        BoardSocketOptions socket;
        socket.url = config.socketUrl;
        socket.token = config.token;
        socket.connector = wsConnector_.get();
        socket.onLog = logAs("socket");
        socket.onActivity = onActivity;
        socket.onConnected = onConnected;
        socket_ = std::make_unique<BoardSocket>(std::move(socket));
        sources.push_back(socket_.get());
    }
    events_ = std::make_unique<CompositeEventSource>(std::move(sources));

    model_ = std::make_unique<AppModel>(*api_, *store_, *pool_, ui, events_.get(),
                                        std::move(config.sync), std::move(config.write));
    modelRef_ = model_.get();
}

AppContext::~AppContext() {
    modelRef_ = nullptr;
    if (socket_) socket_->stop();    // nor a socket callback
    if (webhook_) webhook_->stop();  // deletes the webhook: needs the network, so before shutdown
    poller_->stop();      // no poll callback can still be running (or start) from here on
    model_->quiesce();    // from now on nothing is reported to the UI
    scheduler_->shutdown();  // queued requests fail at once...
    http_->shutdown();       // ...and transfers in flight are aborted: no waiting on the network
    imageHttp_->shutdown();  // same for pictures; then the cache's threads end at once
    images_.reset();
    model_.reset();       // jobs now end quickly; the engine unwatches through the poller
}

}  // namespace cardinal
