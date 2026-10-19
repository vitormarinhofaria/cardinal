#include <gtest/gtest.h>

#include <cstdlib>
#include <unistd.h>

#include "cardinal/credentials.h"

using namespace cardinal;

namespace {

class MemoryStore : public CredentialStore {
public:
    std::optional<Credentials> load() override { return value; }
    void save(const Credentials& c) override { value = c; }
    void clear() override {
        value.reset();
        ++clears;
    }
    std::optional<Credentials> value;
    int clears = 0;
};

}  // namespace

TEST(AuthSession, ReportsStoredTokenAndLogsIn) {
    MemoryStore store;
    AuthSession session(store, [] {});
    EXPECT_FALSE(session.token().has_value());
    session.login("abc123");
    EXPECT_EQ(session.token(), "abc123");
    EXPECT_EQ(store.value->token, "abc123");
}

TEST(AuthSession, InvalidTokenClearsAndRequestsLoginOnce) {
    MemoryStore store;
    store.value = Credentials{"stale"};
    int asked = 0;
    AuthSession session(store, [&] { ++asked; });

    session.onSyncError({401, "invalid token"});
    EXPECT_FALSE(session.token().has_value());
    EXPECT_EQ(asked, 1);

    session.onSyncError({401, "invalid token"});  // ten queued requests fail the same way
    EXPECT_EQ(asked, 1);
    EXPECT_EQ(store.clears, 1);
}

TEST(AuthSession, OtherErrorsKeepTheToken) {
    MemoryStore store;
    store.value = Credentials{"good"};
    int asked = 0;
    AuthSession session(store, [&] { ++asked; });
    for (int status : {0, 404, 429, 500, 503}) session.onSyncError({status, "x"});
    EXPECT_EQ(session.token(), "good");
    EXPECT_EQ(asked, 0);
}

TEST(AuthSession, LoginAfterRejectionWorksAgainAndCanBeRejectedAgain) {
    MemoryStore store;
    int asked = 0;
    AuthSession session(store, [&] { ++asked; });
    session.login("one");
    session.onSyncError({401, ""});
    session.login("two");
    EXPECT_EQ(session.token(), "two");
    session.onSyncError({401, ""});
    EXPECT_EQ(asked, 2);
}

TEST(AuthSession, LogoutClearsWithoutAskingForLogin) {
    MemoryStore store;
    int asked = 0;
    AuthSession session(store, [&] { ++asked; });
    session.login("one");
    session.logout();
    EXPECT_FALSE(session.token().has_value());
    EXPECT_EQ(asked, 0);
}

// Touches the real Secret Service (may prompt to unlock the keyring): opt-in.
TEST(LibsecretStore, RoundTripsAndClears) {
    if (!std::getenv("CARDINAL_TEST_KEYRING")) GTEST_SKIP() << "set CARDINAL_TEST_KEYRING=1";
    auto store = makeLibsecretCredentialStore("cardinal-test-" + std::to_string(::getpid()));
    EXPECT_FALSE(store->load().has_value());
    store->save({"tok-é-☕"});
    ASSERT_TRUE(store->load().has_value());
    EXPECT_EQ(store->load()->token, "tok-é-☕");
    store->save({"second"});
    EXPECT_EQ(store->load()->token, "second");
    store->clear();
    EXPECT_FALSE(store->load().has_value());
    store->clear();  // clearing twice is harmless
}

TEST(CredentialEncoding, TokenOnlyStaysPlainForBackwardCompatibility) {
    EXPECT_EQ(encodeCredentials({"tok123", ""}), "tok123");
    auto back = decodeCredentials("tok123");
    EXPECT_EQ(back.token, "tok123");
    EXPECT_EQ(back.apiKey, "");
}

TEST(CredentialEncoding, KeyAndTokenRoundTrip) {
    Credentials c{"tok-é-☕", "abcdef0123456789abcdef0123456789"};
    auto back = decodeCredentials(encodeCredentials(c));
    EXPECT_EQ(back.token, c.token);
    EXPECT_EQ(back.apiKey, c.apiKey);
}

TEST(CredentialEncoding, GarbageFallsBackToATokenOnlyInsteadOfThrowing) {
    auto back = decodeCredentials("v1\nsó-uma-linha");
    EXPECT_EQ(back.apiKey, "");
    EXPECT_FALSE(back.token.empty());
}
