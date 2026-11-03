#include "cardinal/event_source.h"

#include <algorithm>

namespace cardinal {

std::string PollingOptions::isoNow() { return isoUtc(std::time(nullptr)); }

PollingEventSource::PollingEventSource(TrelloApi& api, Store& store, PollingOptions options)
    : api_(api), store_(store), options_(std::move(options)), interval_(options_.interval) {
    if (options_.autoStart) thread_ = std::thread([this] { run(); });
}

PollingEventSource::~PollingEventSource() { stop(); }

void PollingEventSource::stop() {
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

void PollingEventSource::setInterval(std::chrono::milliseconds interval) {
    {
        std::lock_guard lk(mutex_);
        if (interval == interval_) return;
        interval_ = interval;
        intervalChanged_ = true;
    }
    wake_.notify_all();
}

void PollingEventSource::pollOnce() {
    std::map<std::string, Handler> snapshot;
    {
        std::lock_guard lk(mutex_);
        snapshot = watched_;
    }
    for (const auto& [boardId, handler] : snapshot) {
        try {
            if (pollBoard(boardId, handler) && options_.onSuccess) options_.onSuccess();
        } catch (const std::exception& e) {
            if (options_.onError) options_.onError(e);
        }
    }
}

bool PollingEventSource::pollBoard(const std::string& boardId, const Handler& handler) {
    auto cursor = store_.syncCursor(boardId);
    if (!cursor) {
        store_.setSyncCursor(boardId, options_.nowIso());  // nothing to diff against yet
        return false;                                       // and no network evidence either
    }
    auto actions = api_.actionsSince(boardId, *cursor).get();
    if (actions.empty()) return true;

    std::stable_sort(actions.begin(), actions.end(),
                     [](const Action& a, const Action& b) { return a.date < b.date; });
    const std::string newest = actions.back().date;
    handler(std::move(actions));  // throws => cursor stays, batch comes again
    store_.setSyncCursor(boardId, newest);
    return true;
}

void PollingEventSource::run() {
    std::unique_lock lk(mutex_);
    while (!stop_) {
        intervalChanged_ = false;
        if (wake_.wait_for(lk, interval_, [this] { return stop_ || intervalChanged_; })) {
            if (stop_) break;
            continue;  // new interval: start waiting again from now
        }
        lk.unlock();
        pollOnce();
        lk.lock();
    }
}

}  // namespace cardinal
