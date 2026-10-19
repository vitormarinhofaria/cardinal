#pragma once

#include <memory>
#include <string>

#include "app_page.h"
#include "cardinal/session.h"
#include "login_page.h"

namespace cardinal::gtk {

// Top-level window: shows the login page, a spinner, or the signed-in app depending on Session.
class MainWindow {
public:
    MainWindow(AdwApplication* app, Session& session, std::string dataDir);
    ~MainWindow();
    GtkWindow* window() const { return GTK_WINDOW(window_); }

private:
    void onSessionChanged(const SessionState& state);

    Session& session_;
    std::string dataDir_;
    GtkWidget* window_;
    GtkWidget* stack_;
    std::unique_ptr<LoginPage> login_;
    std::unique_ptr<AppPage> app_;
    int connection_ = 0;
};

}  // namespace cardinal::gtk
