#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "win_util.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using cardinal::SessionState;
using namespace cardinal::win;

namespace winrt::Cardinal::implementation {

MainWindow::MainWindow() {
    InitializeComponent();
    AppWindow().Resize({1280, 800});

    ProgressRing ring;
    ring.IsActive(true);
    ring.Width(32);
    ring.Height(32);
    ring.HorizontalAlignment(HorizontalAlignment::Center);
    ring.VerticalAlignment(VerticalAlignment::Center);
    starting_ = ring;
}

void MainWindow::attach(cardinal::win::Shell& shell) {
    shell_ = &shell;
    auto& session = *shell.session;
    login_ = std::make_unique<LoginPage>(session);
    connection_ = session.state().changed.connect([this](const SessionState& s) { onSessionChanged(s); });
    onSessionChanged(session.state().get());
}

void MainWindow::detach() {
    if (!shell_) return;
    shell_->session->state().changed.disconnect(connection_);
    host().Children().Clear();
    app_ = nullptr;
    login_.reset();
    shell_ = nullptr;
}

void MainWindow::showPage(UIElement const& page) {
    host().Children().Clear();
    host().Children().Append(page);
}

void MainWindow::onSessionChanged(const SessionState& state) {
    using Phase = SessionState::Phase;
    auto& session = *shell_->session;
    if (state.phase != Phase::Ready && app_) {
        // Drop every view-model right away; the session destroys the backend on its next turn.
        showPage(login_->widget());
        app_ = nullptr;
    }
    switch (state.phase) {
        case Phase::Starting:
            showPage(starting_);
            break;
        case Phase::SignedOut:
        case Phase::WaitingForBrowser:
            login_->update(state);
            showPage(login_->widget());
            break;
        case Phase::Ready:
            if (!app_ && session.model()) {
                auto page = vstack(12);
                page.HorizontalAlignment(HorizontalAlignment::Center);
                page.VerticalAlignment(VerticalAlignment::Center);
                page.Children().Append(text("Conectado ao Trello.", 20));
                auto logout = button("Sair");
                logout.HorizontalAlignment(HorizontalAlignment::Center);
                logout.Click([this](auto&&, auto&&) { shell_->session->logout(); });
                page.Children().Append(logout);
                app_ = page;
                showPage(app_);
            }
            break;
    }
}

}  // namespace winrt::Cardinal::implementation
