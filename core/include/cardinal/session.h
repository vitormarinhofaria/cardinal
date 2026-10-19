#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cardinal/app_context.h"
#include "cardinal/auth_flow.h"
#include "cardinal/credentials.h"
#include "cardinal/viewmodel.h"

namespace cardinal {

enum class SessionError {
    None,
    MissingApiKey,   // the build has no Trello API key
    BrowserFailed,   // could not open the browser
    LoginTimedOut,
    InvalidToken,    // pasted token is empty or malformed
    SessionExpired,  // Trello rejected the saved token
};

struct SessionState {
    enum class Phase { Starting, SignedOut, WaitingForBrowser, Ready };
    Phase phase = Phase::Starting;
    SessionError error = SessionError::None;
    bool operator==(const SessionState&) const = default;
};

// Who is signed in, and the AppContext that exists only while someone is. Everything on the UI
// thread. UIs watch state() and build or drop their view-models as the phase changes:
// on leaving Ready, drop every view-model at once; the AppContext is destroyed right after.
class Session {
public:
    Session(AppConfig base, CredentialStore& credentials, BrowserOpener& browser, UiDispatcher& ui,
            AuthOptions auth = {});
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Observable<SessionState>& state() { return state_; }
    AppModel* model() { return context_ ? &context_->model() : nullptr; }

    void startLogin();                          // browser authorization
    void cancelLogin();
    void loginWithToken(const std::string& token);  // pasted by hand
    void logout();

private:
    void becomeReady(const std::string& token);
    void signOut(SessionError why, bool clearCredentials);
    void background(std::function<void()> work);

    AppConfig base_;
    CredentialStore& credentials_;
    BrowserOpener& browser_;
    UiDispatcher& ui_;
    AuthOptions authOptions_;

    Observable<SessionState> state_;
    std::unique_ptr<AppContext> context_;
    int statusConnection_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // read on the UI thread only
    std::unique_ptr<AuthFlow> flow_;
    std::mutex threadsMutex_;
    std::vector<std::thread> threads_;
    std::atomic<bool> closing_{false};
};

}  // namespace cardinal
