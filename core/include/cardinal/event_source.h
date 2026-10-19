#pragma once

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cardinal/model.h"
#include "cardinal/store.h"
#include "cardinal/time_util.h"
#include "cardinal/trello_api.h"

namespace cardinal {

// Source of board activity. The handler receives actions oldest first; if it throws, the batch is
// considered not applied and is delivered again later.
class EventSource {
public:
    using Handler = std::function<void(std::vector<Action>)>;
    virtual ~EventSource() = default;
    virtual void watch(const std::string& boardId, Handler handler) = 0;
    virtual void unwatch(const std::string& boardId) = 0;
};

struct PollingOptions {
    std::chrono::milliseconds interval{20000};
    std::function<std::string()> nowIso = [] { return isoNow(); };
    std::function<void(const std::exception&)> onError;  // called on the polling thread
    std::function<void()> onSuccess;                     // a poll was answered, with or without news
    bool autoStart = true;                               // false: tests call pollOnce()

    static std::string isoNow();
};

// Polls /boards/{id}/actions?since=<cursor> for every watched board.
class PollingEventSource : public EventSource {
public:
    PollingEventSource(TrelloApi& api, Store& store, PollingOptions options = {});
    ~PollingEventSource() override;
    PollingEventSource(const PollingEventSource&) = delete;
    PollingEventSource& operator=(const PollingEventSource&) = delete;

    void watch(const std::string& boardId, Handler handler) override;
    void unwatch(const std::string& boardId) override;

    void pollOnce();
    // Stops the polling thread (and with it every callback); the object stays usable for unwatch().
    void stop();

private:
    bool pollBoard(const std::string& boardId, const Handler& handler);  // true: Trello answered
    void run();

    TrelloApi& api_;
    Store& store_;
    PollingOptions options_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::string, Handler> watched_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace cardinal
