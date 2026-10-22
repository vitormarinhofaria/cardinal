#include "pch.h"

#include "platform.h"

#include <shellapi.h>

#include <stdexcept>

#include "win_util.h"

namespace cardinal::win {

WinDispatcher::WinDispatcher()
    : queue_(winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread()) {
    if (!queue_) throw std::logic_error("WinDispatcher must be created on the UI thread");
}

void WinDispatcher::post(std::function<void()> fn) {
    queue_.TryEnqueue([fn = std::move(fn)] { fn(); });
}

void WinBrowserOpener::open(const std::string& url) {
    // Only web links: ShellExecute would happily run a local program.
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0)
        throw std::runtime_error("refusing to open a non-web URL");
    auto result = reinterpret_cast<INT_PTR>(
        ::ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) throw std::runtime_error("ShellExecute failed: " + std::to_string(result));
}

}  // namespace cardinal::win
