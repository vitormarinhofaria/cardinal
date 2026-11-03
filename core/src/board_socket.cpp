#include "cardinal/board_socket.h"

#include "cardinal/debouncer.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <optional>

namespace cardinal {
namespace {

using SteadyClock = std::chrono::steady_clock;

std::string request(const char* type, int reqid, const std::string& boardId = "") {
    nlohmann::json j{{"type", type}, {"reqid", reqid}};
    if (!boardId.empty()) {
        j["modelType"] = "Board";
        j["idModel"] = boardId;
        j["tags"] = {"clientActions", "updates"};
        j["invitationTokens"] = nlohmann::json::array();
    }
    return j.dump();
}

}  // namespace

BoardSocket::BoardSocket(BoardSocketOptions options) : options_(std::move(options)) {
    if (options_.autoStart) start();
}

BoardSocket::~BoardSocket() { stop(); }

void BoardSocket::start() {
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
}

void BoardSocket::stop() {
    {
        std::lock_guard lk(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void BoardSocket::watch(const std::string& boardId, Handler) {
    {
        std::lock_guard lk(mutex_);
        board_ = boardId;
    }
    wake_.notify_all();
}

void BoardSocket::unwatch(const std::string& boardId) {
    std::lock_guard lk(mutex_);
    if (board_ == boardId) board_.clear();
}

bool BoardSocket::isNotification(const std::string& message) {
    return !message.empty() && message.find("\"reqid\"") == std::string::npos;
}

std::string BoardSocket::wanted() {
    std::lock_guard lk(mutex_);
    return board_;
}

bool BoardSocket::stopping() {
    std::lock_guard lk(mutex_);
    return stop_;
}

void BoardSocket::log(const std::string& line) {
    if (options_.onLog) options_.onLog(line);
}

void BoardSocket::run() {
    auto backoff = options_.minBackoff;
    bool everConnected = false;
    for (;;) {
        {
            std::unique_lock lk(mutex_);
            wake_.wait(lk, [this] { return stop_ || !board_.empty(); });  // connect when needed
            if (stop_) return;
        }
        healthy_ = false;
        try {
            std::vector<std::string> headers{"Cookie: token=" + options_.token,
                                             "Origin: https://trello.com"};
            const std::string sep = options_.url.find('?') == std::string::npos ? "?" : "&";
            auto conn = options_.connector->connect(options_.url + sep + "token=" + options_.token,
                                                    headers, [this] { return stopping(); });
            backoff = options_.minBackoff;
            log("connected");
            serve(*conn, everConnected);
        } catch (const Rejected& e) {
            // Trello will not take this token on the socket; retrying would only repeat that.
            log(std::string("subscription refused (") + e.what() + "): live push off, polling only");
            if (healthy_ && options_.onConnected) options_.onConnected(false);
            std::unique_lock lk(mutex_);
            wake_.wait(lk, [this] { return stop_; });
            return;
        } catch (const std::exception& e) {
            log(std::string("disconnected: ") + e.what());
        }
        if (healthy_) {
            everConnected = true;
            if (options_.onConnected) options_.onConnected(false);
        }
        std::unique_lock lk(mutex_);
        if (wake_.wait_for(lk, backoff, [this] { return stop_; })) return;
        backoff = std::min(backoff * 2, options_.maxBackoff);
    }
}

// Returns only on stop(); a broken connection throws.
void BoardSocket::serve(WebSocketConnection& conn, bool catchUp) {
    int reqid = 0;
    int subscribeReqid = -1;  // the subscribe still waiting for Trello's answer
    std::string subscribed;
    auto lastPing = SteadyClock::now();
    Debouncer pending(options_.quiet, options_.maxDelay);  // notifications not yet reported

    auto send = [&](const std::string& text) {
        log("-> " + text);
        conn.send(text);
    };
    auto fire = [&](const std::string& board) {
        pending.reset();
        log("board changed: reloading " + board);
        if (options_.onActivity) options_.onActivity(board);
    };

    while (!stopping()) {
        const std::string want = wanted();
        if (want != subscribed) {
            if (!subscribed.empty()) send(request("unsubscribe", reqid++, subscribed));
            pending.reset();
            if (!want.empty()) {
                subscribeReqid = reqid;
                send(request("subscribe", reqid++, want));
                log("subscribed to " + want);
                if (catchUp) fire(want);  // events may have been missed while disconnected
            }
            catchUp = false;
            subscribed = want;
        }
        if (SteadyClock::now() - lastPing >= options_.pingEvery) {
            send(request("ping", reqid++));
            lastPing = SteadyClock::now();
        }

        auto msg = conn.receive(options_.tick);
        if (msg) log("<- " + *msg);
        if (msg && subscribeReqid >= 0 && !isNotification(*msg)) {
            // Replies are the only messages read: did Trello accept the subscription?
            auto reply = nlohmann::json::parse(*msg, nullptr, false);
            if (reply.is_object() && reply.value("reqid", -1) == subscribeReqid) {
                subscribeReqid = -1;
                if (reply.contains("error")) throw Rejected(reply["error"].dump());
                if (!healthy_) {
                    healthy_ = true;
                    if (options_.onConnected) options_.onConnected(true);
                }
            }
        }
        const auto now = SteadyClock::now();
        if (msg && !subscribed.empty() && isNotification(*msg)) pending.note(now);
        if (pending.due(now)) fire(subscribed);
    }
}

}  // namespace cardinal
