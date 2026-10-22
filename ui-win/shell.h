#pragma once

#include <memory>
#include <string>

#include "cardinal/credentials.h"
#include "cardinal/session.h"
#include "platform.h"

namespace cardinal::win {

// Development switches, so the app can run against tools/mock_trello.py with no real account.
// Same names as ui-gtk: --api-base, --api-key, --token, --data-dir, --poll-seconds, --no-keyring,
// --open-card. Both "--name value" and "--name=value" work.
struct Options {
    std::string apiBase;
    std::string apiKey;
    std::string token;
    std::string dataDir;
    int pollSeconds = 20;
    bool noKeyring = false;  // keep credentials in memory only (development)
    std::string openCard;    // open this card as soon as the board shows (development)
};

Options parseCommandLine();

// Everything that outlives a window: the dispatcher, the secret store and the Session. Built on
// the UI thread; destroyed after the window dropped its pages (view-models go before the model).
struct Shell {
    explicit Shell(Options options);
    ~Shell();

    Options options;
    std::string dataDir;
    WinDispatcher dispatcher;
    WinBrowserOpener browser;
    std::unique_ptr<CredentialStore> credentials;
    std::unique_ptr<Session> session;
};

}  // namespace cardinal::win
