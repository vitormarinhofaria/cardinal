#include "cardinal/credentials.h"

namespace cardinal {

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
