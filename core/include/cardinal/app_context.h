#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "cardinal/event_source.h"
#include "cardinal/executor.h"
#include "cardinal/http.h"
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

private:
    // Declaration order is destruction order in reverse: model first, store last.
    std::unique_ptr<Store> store_;
    std::unique_ptr<HttpClient> http_;
    SystemClock clock_;
    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<TrelloApi> api_;
    std::unique_ptr<ThreadPoolExecutor> pool_;
    std::unique_ptr<PollingEventSource> poller_;
    std::unique_ptr<AppModel> model_;
};

}  // namespace cardinal
