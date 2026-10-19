#pragma once

#include "cardinal/auth_flow.h"
#include "cardinal/viewmodel.h"

namespace cardinal::gtk {

// Runs callbacks on the GTK main loop, always deferred (never re-entrant).
class GtkDispatcher : public UiDispatcher {
public:
    void post(std::function<void()> fn) override;
};

// Opens the user's default browser.
class GtkBrowserOpener : public BrowserOpener {
public:
    void open(const std::string& url) override;
};

}  // namespace cardinal::gtk
