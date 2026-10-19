#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "cardinal/sync.h"

namespace cardinal {

struct Credentials {
    std::string token;   // the user's Trello token
    std::string apiKey = "";  // the key that token was issued for; empty = the app's built-in key
};

// How a store keeps both in one secret. A token alone is stored as is (older entries keep working).
std::string encodeCredentials(const Credentials& c);
Credentials decodeCredentials(const std::string& secret);

// Secret storage. Implementations: libsecret (Linux); Keychain / Credential Manager elsewhere.
class CredentialStore {
public:
    virtual ~CredentialStore() = default;
    virtual std::optional<Credentials> load() = 0;
    virtual void save(const Credentials& credentials) = 0;
    virtual void clear() = 0;
};

#if defined(_WIN32)
// Windows Credential Manager (generic credential). `account` separates entries, e.g. in tests.
std::unique_ptr<CredentialStore> makeWinCredCredentialStore(std::string account = "default");
#else
// Secret Service (GNOME Keyring, KWallet). `account` separates entries, e.g. in tests.
std::unique_ptr<CredentialStore> makeLibsecretCredentialStore(std::string account = "default");
#endif

// Knows whether the user is signed in and reacts to Trello rejecting the token. Thread-safe.
class AuthSession {
public:
    AuthSession(CredentialStore& store, std::function<void()> onLoginRequired);

    std::optional<std::string> token();
    void login(const std::string& token);
    void logout();

    // Feed sync/write errors here: a 401 clears the token and asks for a new login (once).
    // The local cache is left alone so the app keeps showing data.
    void onSyncError(const SyncError& error);

private:
    CredentialStore& store_;
    std::function<void()> onLoginRequired_;
    std::mutex mutex_;
};

}  // namespace cardinal
