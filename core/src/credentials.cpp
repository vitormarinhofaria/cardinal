#include "cardinal/credentials.h"

namespace cardinal {

std::string encodeCredentials(const Credentials& c) {
    if (c.apiKey.empty()) return c.token;
    return "v1\n" + c.apiKey + "\n" + c.token;
}

Credentials decodeCredentials(const std::string& secret) {
    if (secret.rfind("v1\n", 0) == 0) {
        const auto second = secret.find('\n', 3);
        if (second != std::string::npos) return {secret.substr(second + 1), secret.substr(3, second - 3)};
    }
    return {secret, ""};  // plain token (older entries) or something we do not recognize
}

AuthSession::AuthSession(CredentialStore& store, std::function<void()> onLoginRequired)
    : store_(store), onLoginRequired_(std::move(onLoginRequired)) {}

std::optional<std::string> AuthSession::token() {
    std::lock_guard lk(mutex_);
    auto c = store_.load();
    if (!c) return std::nullopt;
    return c->token;
}

void AuthSession::login(const std::string& token) {
    std::lock_guard lk(mutex_);
    store_.save({token});
}

void AuthSession::logout() {
    std::lock_guard lk(mutex_);
    store_.clear();
}

void AuthSession::onSyncError(const SyncError& error) {
    if (error.status != 401) return;
    {
        std::lock_guard lk(mutex_);
        if (!store_.load()) return;  // already signed out: the login prompt is already up
        store_.clear();
    }
    if (onLoginRequired_) onLoginRequired_();
}

}  // namespace cardinal
