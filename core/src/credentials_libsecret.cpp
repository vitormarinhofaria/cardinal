#include <libsecret/secret.h>

#include <stdexcept>

#include "cardinal/credentials.h"

namespace cardinal {
namespace {

const SecretSchema* schema() {
    static const SecretSchema s = [] {
        SecretSchema v{};
        v.name = "dev.cardinal.TrelloToken";
        v.flags = SECRET_SCHEMA_NONE;
        v.attributes[0] = {"account", SECRET_SCHEMA_ATTRIBUTE_STRING};
        return v;
    }();
    return &s;
}

void check(GError* error, const char* what) {
    if (!error) return;
    std::string msg = std::string(what) + ": " + error->message;
    g_error_free(error);
    throw std::runtime_error(msg);
}

class LibsecretStore : public CredentialStore {
public:
    explicit LibsecretStore(std::string account) : account_(std::move(account)) {}

    std::optional<Credentials> load() override {
        GError* error = nullptr;
        gchar* secret = secret_password_lookup_sync(schema(), nullptr, &error, "account",
                                                    account_.c_str(), nullptr);
        check(error, "keyring lookup");
        if (!secret) return std::nullopt;
        Credentials c{secret};
        secret_password_free(secret);
        return c;
    }

    void save(const Credentials& c) override {
        GError* error = nullptr;
        secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, "Cardinal (Trello token)",
                                   c.token.c_str(), nullptr, &error, "account", account_.c_str(),
                                   nullptr);
        check(error, "keyring store");
    }

    void clear() override {
        GError* error = nullptr;
        secret_password_clear_sync(schema(), nullptr, &error, "account", account_.c_str(), nullptr);
        check(error, "keyring clear");
    }

private:
    std::string account_;
};

}  // namespace

std::unique_ptr<CredentialStore> makeLibsecretCredentialStore(std::string account) {
    return std::make_unique<LibsecretStore>(std::move(account));
}

}  // namespace cardinal
