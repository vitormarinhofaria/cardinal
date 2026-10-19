#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace cardinal {

class AuthError : public std::runtime_error {
public:
    enum class Kind { TimedOut, Cancelled, BrowserFailed, ListenFailed };
    AuthError(Kind kind, const std::string& what) : std::runtime_error(what), kind_(kind) {}
    Kind kind() const { return kind_; }

private:
    Kind kind_;
};

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

// The Trello page where a signed-in user authorizes `apiKey` and is shown a token to copy. Used when
// the automatic flow is not available (key not whitelisted for the loopback address).
std::string manualAuthorizeUrl(const std::string& apiKey, const std::string& appName = "Cardinal");

// Trello's browser authorization without a backend: the user approves in the browser, Trello
// redirects to a page served on 127.0.0.1 with the token in the URL fragment, a tiny script on
// that page posts it back to this process. One attempt; the server closes when done.
class AuthFlow {
public:
    AuthFlow(std::string apiKey, BrowserOpener& opener, AuthOptions options = {});
    ~AuthFlow();  // cancels a pending flow
    AuthFlow(const AuthFlow&) = delete;
    AuthFlow& operator=(const AuthFlow&) = delete;

    // The future yields the token, or throws AuthError (timeout, cancelled, browser could not be
    // opened, port unavailable).
    std::future<std::string> run();

private:
    void serve(std::uintptr_t listenHandle, unsigned short port, std::string nonce,
               std::shared_ptr<std::promise<std::string>> promise);

    std::string apiKey_;
    BrowserOpener& opener_;
    AuthOptions options_;
    std::atomic<bool> stop_{false};
    bool started_ = false;
    std::thread thread_;
};

}  // namespace cardinal
