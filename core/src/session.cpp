#include "cardinal/session.h"

#include <algorithm>

namespace cardinal {
namespace {

std::string trim(const std::string& s) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    auto b = std::find_if_not(s.begin(), s.end(), isSpace);
    auto e = std::find_if_not(s.rbegin(), std::string::const_reverse_iterator(b), isSpace).base();
    return std::string(b, e);
}

// Trello API keys are 32 hex characters; accept any plain alphanumeric string of a sane length.
bool looksLikeApiKey(const std::string& k) {
    if (k.size() < 16 || k.size() > 64) return false;
    return std::all_of(k.begin(), k.end(), [](unsigned char c) { return std::isalnum(c) != 0; });
}

bool looksLikeToken(const std::string& t) {
    if (t.size() < 16 || t.size() > 512) return false;
    return std::all_of(t.begin(), t.end(),
                       [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
}

}  // namespace

Session::Session(AppConfig base, CredentialStore& credentials, BrowserOpener& browser,
                 UiDispatcher& ui, AuthOptions auth)
    : base_(std::move(base)),
      credentials_(credentials),
      browser_(browser),
      ui_(ui),
      authOptions_(std::move(auth)),
      state_(SessionState{}) {
    background([this, alive = alive_] {
        std::optional<Credentials> saved;
        try {
            saved = credentials_.load();
        } catch (const std::exception&) {  // keyring unavailable: behave as signed out
        }
        ui_.post([this, alive, saved = std::move(saved)] {
            if (!*alive) return;
            if (saved && looksLikeToken(saved->token) && (saved->apiKey.empty() || looksLikeApiKey(saved->apiKey)))
                becomeReady(saved->token, saved->apiKey);
            else
                state_.set({SessionState::Phase::SignedOut, SessionError::None});
        });
    });
}

Session::~Session() {
    *alive_ = false;
    closing_ = true;
    flow_.reset();  // wakes a thread waiting for the browser
    std::vector<std::thread> threads;
    {
        std::lock_guard lk(threadsMutex_);
        threads.swap(threads_);
    }
    for (auto& t : threads) t.join();
    context_.reset();
}

void Session::background(std::function<void()> work) {
    std::lock_guard lk(threadsMutex_);
    if (closing_) return;
    threads_.emplace_back(std::move(work));
}

void Session::startLogin() {
    if (state_.get().phase == SessionState::Phase::WaitingForBrowser) return;
    if (base_.apiKey.empty()) {
        state_.set({SessionState::Phase::SignedOut, SessionError::MissingApiKey});
        return;
    }
    flow_ = std::make_unique<AuthFlow>(base_.apiKey, browser_, authOptions_);
    auto result = std::make_shared<std::future<std::string>>(flow_->run());
    state_.set({SessionState::Phase::WaitingForBrowser, SessionError::None});

    background([this, alive = alive_, result] {
        std::string token;
        SessionError error = SessionError::None;
        try {
            token = result->get();
        } catch (const AuthError& e) {
            switch (e.kind()) {
                case AuthError::Kind::TimedOut: error = SessionError::LoginTimedOut; break;
                case AuthError::Kind::Cancelled: return;  // the user (or shutdown) asked for it
                default: error = SessionError::BrowserFailed; break;
            }
        } catch (const std::exception&) {
            error = SessionError::BrowserFailed;
        }
        ui_.post([this, alive, token, error] {
            if (!*alive || state_.get().phase != SessionState::Phase::WaitingForBrowser) return;
            flow_.reset();
            if (error != SessionError::None) {
                state_.set({SessionState::Phase::SignedOut, error});
                return;
            }
            background([this, token] {
                try {
                    credentials_.save({token, ""});
                } catch (const std::exception&) {  // no keyring: the session still works until exit
                }
            });
            becomeReady(token, "");
        });
    });
}

void Session::cancelLogin() {
    if (state_.get().phase != SessionState::Phase::WaitingForBrowser) return;
    flow_.reset();
    state_.set({SessionState::Phase::SignedOut, SessionError::None});
}

void Session::openTokenPage(const std::string& typedKey) {
    std::string key = trim(typedKey);
    const bool typed = !key.empty();
    if (!typed) key = base_.apiKey;  // the build's own key is trusted as is
    if (key.empty()) {
        state_.set({state_.get().phase, SessionError::MissingApiKey});
        return;
    }
    if (typed && !looksLikeApiKey(key)) {
        state_.set({state_.get().phase, SessionError::InvalidApiKey});
        return;
    }
    try {
        browser_.open(manualAuthorizeUrl(key));
        if (state_.get().error != SessionError::None) state_.set({state_.get().phase, SessionError::None});
    } catch (const std::exception&) {
        state_.set({state_.get().phase, SessionError::BrowserFailed});
    }
}

void Session::loginWithToken(const std::string& raw, const std::string& rawKey) {
    const std::string token = trim(raw);
    const std::string apiKey = trim(rawKey);
    if (state_.get().phase == SessionState::Phase::WaitingForBrowser) {
        flow_.reset();
    }
    if (!apiKey.empty() && !looksLikeApiKey(apiKey)) {
        state_.set({SessionState::Phase::SignedOut, SessionError::InvalidApiKey});
        return;
    }
    if (!looksLikeToken(token)) {
        state_.set({SessionState::Phase::SignedOut, SessionError::InvalidToken});
        return;
    }
    background([this, token, apiKey] {
        try {
            credentials_.save({token, apiKey});
        } catch (const std::exception&) {
        }
    });
    becomeReady(token, apiKey);
}

void Session::logout() { signOut(SessionError::None, true); }

void Session::becomeReady(const std::string& token, const std::string& apiKey) {
    AppConfig cfg = base_;
    cfg.token = token;
    if (!apiKey.empty()) cfg.apiKey = apiKey;  // the token was issued for this key
    context_ = std::make_unique<AppContext>(std::move(cfg), ui_);
    statusConnection_ = context_->model().status().changed.connect([this](const AppStatus& s) {
        if (s.loginRequired) signOut(SessionError::SessionExpired, true);
    });
    state_.set({SessionState::Phase::Ready, SessionError::None});
}

void Session::signOut(SessionError why, bool clearCredentials) {
    if (context_) {
        context_->model().status().changed.disconnect(statusConnection_);
        // The UI drops its view-models while handling the state change below; the context itself
        // goes away on the next UI turn (we may be inside one of its own signals right now).
        // Tearing it down joins worker threads: do that away from the UI thread.
        std::shared_ptr<AppContext> old(std::move(context_));
        background([old = std::move(old)]() mutable { old.reset(); });
    }
    if (clearCredentials) {
        background([this] {
            try {
                credentials_.clear();
            } catch (const std::exception&) {
            }
        });
    }
    state_.set({SessionState::Phase::SignedOut, why});
}

}  // namespace cardinal
