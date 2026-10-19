#include "cardinal/app_context.h"

namespace cardinal {

AppContext::AppContext(AppConfig config, UiDispatcher& ui) {
    std::filesystem::create_directories(config.dataDir);
    store_ = std::make_unique<Store>(Store::open(config.dataDir / "cache.db"));
    http_ = makeCurlHttpClient(config.workerThreads);
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

    model_ = std::make_unique<AppModel>(*api_, *store_, *pool_, ui, poller_.get(),
                                        std::move(config.sync), std::move(config.write));
    modelRef_ = model_.get();
}

AppContext::~AppContext() {
    modelRef_ = nullptr;
    poller_->stop();  // no poll callback can still be running (or start) from here on
    model_.reset();   // the engine unwatches through the poller, so the poller outlives the model
}

}  // namespace cardinal
