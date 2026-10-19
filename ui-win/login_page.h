#pragma once

#include "cardinal/session.h"

namespace cardinal::win {

// "Entrar com o Trello" and manual token entry. Pure view: all logic is in Session.
class LoginPage {
public:
    explicit LoginPage(Session& session);
    winrt::Microsoft::UI::Xaml::UIElement widget() const { return root_; }
    void update(const SessionState& state);

private:
    Session& session_;
    winrt::Microsoft::UI::Xaml::Controls::ScrollViewer root_;
    winrt::Microsoft::UI::Xaml::Controls::Button signIn_;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel waiting_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::InfoBar error_;
    winrt::Microsoft::UI::Xaml::Controls::Expander manual_;
    winrt::Microsoft::UI::Xaml::Controls::TextBox keyEntry_;
    winrt::Microsoft::UI::Xaml::Controls::PasswordBox tokenEntry_;
};

}  // namespace cardinal::win
