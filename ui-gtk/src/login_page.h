#pragma once

#include "cardinal/session.h"
#include "gtk_util.h"

namespace cardinal::gtk {

// "Entrar com o Trello" and manual token entry. Pure view: all logic is in Session.
class LoginPage {
public:
    explicit LoginPage(Session& session);
    GtkWidget* widget() const { return root_; }
    void update(const SessionState& state);

private:
    Session& session_;
    GtkWidget* root_;
    GtkWidget* signIn_;
    GtkWidget* waiting_;
    GtkWidget* error_;
    GtkWidget* keyEntry_;
    GtkWidget* tokenEntry_;
    GtkWidget* manual_;
};

}  // namespace cardinal::gtk
