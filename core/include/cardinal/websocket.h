#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cardinal {

// One open websocket. Not thread-safe: a single thread sends and receives. Any failure (connection
// lost, close frame from the server) is thrown as std::runtime_error; the connection is then dead.
class WebSocketConnection {
public:
    virtual ~WebSocketConnection() = default;
    virtual void send(const std::string& text) = 0;
    // A whole text message, or nullopt when none arrived within `timeout`.
    virtual std::optional<std::string> receive(std::chrono::milliseconds timeout) = 0;
};

class WebSocketConnector {
public:
    virtual ~WebSocketConnector() = default;
    // Throws when the connection or the upgrade fails. `headers` are "Name: value". `cancelled`
    // (optional) is polled while connecting; returning true aborts the attempt.
    virtual std::unique_ptr<WebSocketConnection> connect(const std::string& url,
                                                         const std::vector<std::string>& headers,
                                                         std::function<bool()> cancelled = {}) = 0;
};

std::unique_ptr<WebSocketConnector> makeCurlWebSocketConnector();

}  // namespace cardinal
