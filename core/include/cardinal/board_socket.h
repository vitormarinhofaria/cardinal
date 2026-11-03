#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "cardinal/event_source.h"
#include "cardinal/websocket.h"

namespace cardinal {

struct BoardSocketOptions {
    std::string url = "wss://trello.com/1/Session/socket";
    std::string token;
    WebSocketConnector* connector = nullptr;  // required; outlives the socket

    // Called on the socket's thread with the watched board whenever Trello says it changed (a
    // burst of notifications is folded into one call), and once after a reconnection, since
    // anything may have happened while the link was down.
    std::function<void(const std::string& boardId)> onActivity;
    // true once Trello accepted a subscription, false when that link is lost (on the socket's
    // thread). A refused subscription (bad token) turns the socket off for good: polling only.
    std::function<void(bool connected)> onConnected;
    std::function<void(const std::string&)> onLog;    // diagnostics, optional: every message in and out

    std::chrono::milliseconds quiet{1000};     // fire once notifications stop for this long...
    std::chrono::milliseconds maxDelay{3000};  // ...or this long after the first one at the latest
    std::chrono::milliseconds pingEvery{30000};
    std::chrono::milliseconds tick{500};       // how soon a board change or stop() is noticed
    std::chrono::milliseconds minBackoff{1000};
    std::chrono::milliseconds maxBackoff{60000};
    bool autoStart = true;
};

// Trello's (undocumented) websocket, used only as a trigger: the content of a notification is not
// read, it only means "this board changed, reload it". Best effort: when the socket cannot connect
// it keeps retrying with backoff and the polling event source carries on alone.
// watch() only records which board to subscribe to; the handler is never called.
class BoardSocket : public EventSource {
public:
    explicit BoardSocket(BoardSocketOptions options);
    ~BoardSocket() override;
    BoardSocket(const BoardSocket&) = delete;
    BoardSocket& operator=(const BoardSocket&) = delete;

    void watch(const std::string& boardId, Handler handler) override;
    void unwatch(const std::string& boardId) override;

    void start();  // only when autoStart is false
    void stop();   // joins the thread; no callback runs afterwards

    // Whether a message counts as a change notification (not a reply to our own requests).
    static bool isNotification(const std::string& message);

private:
    struct Rejected : std::runtime_error {
        using std::runtime_error::runtime_error;
    };
    void run();
    void serve(WebSocketConnection& conn, bool catchUp);
    std::string wanted();
    bool stopping();
    void log(const std::string& line);

    BoardSocketOptions options_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::string board_;
    bool stop_ = false;
    bool healthy_ = false;  // the socket thread's own: a subscription was accepted on this link
    std::thread thread_;
};

}  // namespace cardinal
