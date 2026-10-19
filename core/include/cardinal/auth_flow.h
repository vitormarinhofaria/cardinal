#pragma once

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace cardinal {

// Opens a URL in the user's browser. Implemented by the UI layer.
class BrowserOpener {
public:
    virtual ~BrowserOpener() = default;
    virtual void open(const std::string& url) = 0;  // may throw
};

struct AuthOptions {
    std::string authorizeUrl = "https://trello.com/1/authorize";
    std::string appName = "Cardinal";
    std::chrono::seconds timeout{300};
    unsigned short port = 0;  // 0 = any free port; pin one if the API key whitelists origins
};

// Trello's browser authorization without a backend: the user approves in the browser, Trello
// redirects to a page served on 127.0.0.1 with the token in the URL fragment, a tiny script on
// that page posts it back to this process. One attempt; the server closes when done.
class AuthFlow {
public:
    AuthFlow(std::string apiKey, BrowserOpener& opener, AuthOptions options = {});
    ~AuthFlow();  // cancels a pending flow
    AuthFlow(const AuthFlow&) = delete;
    AuthFlow& operator=(const AuthFlow&) = delete;

    // The future yields the token, or throws std::runtime_error (timeout, cancelled, browser
    // could not be opened).
    std::future<std::string> run();

private:
    void serve(int listenFd, unsigned short port, std::string nonce,
               std::shared_ptr<std::promise<std::string>> promise);

    std::string apiKey_;
    BrowserOpener& opener_;
    AuthOptions options_;
    std::atomic<bool> stop_{false};
    bool started_ = false;
    std::thread thread_;
};

}  // namespace cardinal
