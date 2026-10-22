#include "pch.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::Cardinal::implementation {

App::App() {
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
    UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
        if (IsDebuggerPresent()) {
            auto errorMessage = e.Message();
            __debugbreak();
        }
    });
#endif
}

void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e) {
    shell_ = std::make_unique<cardinal::win::Shell>(cardinal::win::parseCommandLine());
    window_ = make<MainWindow>();
    auto* impl = get_self<MainWindow>(window_.as<Cardinal::MainWindow>());
    impl->attach(*shell_);
    window_.Closed([this](auto&&, auto&&) {
        // Pages and view-models first, then the Session that owns the model.
        get_self<MainWindow>(window_.as<Cardinal::MainWindow>())->detach();
        shell_.reset();
    });
    window_.Activate();
}

}  // namespace winrt::Cardinal::implementation
