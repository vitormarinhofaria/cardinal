#pragma once

#include <string>

#include "cardinal/auth_flow.h"
#include "cardinal/viewmodel.h"

namespace cardinal::win {

// Runs callbacks on the UI thread's DispatcherQueue, always deferred (never re-entrant). Safe to
// call from any thread; posts after the queue shut down are dropped.
class WinDispatcher : public UiDispatcher {
public:
    WinDispatcher();  // call on the UI thread
    void post(std::function<void()> fn) override;

private:
    winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_{nullptr};
};

// Opens the user's default browser.
class WinBrowserOpener : public BrowserOpener {
public:
    void open(const std::string& url) override;
};

}  // namespace cardinal::win
