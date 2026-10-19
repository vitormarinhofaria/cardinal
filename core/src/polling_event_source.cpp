#include "cardinal/event_source.h"

#include <algorithm>

namespace cardinal {

std::string PollingOptions::isoNow() { return isoUtc(std::time(nullptr)); }

PollingEventSource::PollingEventSource(TrelloApi& api, Store& store, PollingOptions options)
    : api_(api), store_(store), options_(std::move(options)) {
    if (options_.autoStart) thread_ = std::thread([this] { run(); });
}

PollingEventSource::~PollingEventSource() {
    {
        std::lock_guard lk(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void PollingEventSource::watch(const std::string& boardId, Handler handler) {
    std::lock_guard lk(mutex_);
    watched_[boardId] = std::move(handler);
}

void PollingEventSource::unwatch(const std::string& boardId) {
    std::lock_guard lk(mutex_);
    watched_.erase(boardId);
}

void PollingEventSource::pollOnce() {
    std::map<std::string, Handler> snapshot;
    {
        std::lock_guard lk(mutex_);
        snapshot = watched_;
    }
    for (const auto& [boardId, handler] : snapshot) {
        try {
            pollBoard(boardId, handler);
        } catch (const std::exception& e) {
            if (options_.onError) options_.onError(e);
        }
    }
}

void PollingEventSource::pollBoard(const std::string& boardId, const Handler& handler) {
    auto cursor = store_.syncCursor(boardId);
    if (!cursor) {
        store_.setSyncCursor(boardId, options_.nowIso());  // nothing to diff against yet
        return;
    }
    auto actions = api_.actionsSince(boardId, *cursor).get();
    if (actions.empty()) return;

    std::stable_sort(actions.begin(), actions.end(),
                     [](const Action& a, const Action& b) { return a.date < b.date; });
    const std::string newest = actions.back().date;
    handler(std::move(actions));  // throws => cursor stays, batch comes again
    store_.setSyncCursor(boardId, newest);
}

void PollingEventSource::run() {
    std::unique_lock lk(mutex_);
    while (!stop_) {
        if (wake_.wait_for(lk, options_.interval, [this] { return stop_; })) break;
        lk.unlock();
        pollOnce();
        lk.lock();
    }
}

}  // namespace cardinal
