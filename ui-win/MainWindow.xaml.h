#pragma once

#include <memory>

#include "MainWindow.g.h"
#include "app_page.h"
#include "login_page.h"
#include "shell.h"

namespace winrt::Cardinal::implementation {

// Top-level window: shows the login page, a spinner, or the signed-in app depending on Session.
struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();

    // Not part of the WinRT surface: App hands the shell over right after creating the window and
    // detaches before destroying it, so every page (and view-model) goes before the Session.
    void attach(cardinal::win::Shell& shell);
    void detach();

private:
    void onSessionChanged(const cardinal::SessionState& state);
    void showPage(Microsoft::UI::Xaml::UIElement const& page);

    cardinal::win::Shell* shell_ = nullptr;
    int connection_ = 0;
    Microsoft::UI::Xaml::UIElement starting_{nullptr};
    std::unique_ptr<cardinal::win::LoginPage> login_;
    std::unique_ptr<cardinal::win::AppPage> app_;
};

}  // namespace winrt::Cardinal::implementation

namespace winrt::Cardinal::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::Cardinal::factory_implementation
