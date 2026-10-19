#include "window.h"

namespace cardinal::gtk {

MainWindow::MainWindow(AdwApplication* app, Session& session, std::string dataDir)
    : session_(session), dataDir_(std::move(dataDir)) {
    window_ = adw_application_window_new(GTK_APPLICATION(app));
    gtk_window_set_title(GTK_WINDOW(window_), "Cardinal");
    gtk_window_set_default_size(GTK_WINDOW(window_), 1280, 800);

    stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack_), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    GtkWidget* starting = adw_status_page_new();
    GtkWidget* spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_size_request(spinner, 32, 32);
    adw_status_page_set_child(ADW_STATUS_PAGE(starting), spinner);
    adw_status_page_set_title(ADW_STATUS_PAGE(starting), "Cardinal");
    gtk_stack_add_named(GTK_STACK(stack_), starting, "starting");

    login_ = std::make_unique<LoginPage>(session_);
    GtkWidget* loginToolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(loginToolbar), adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(loginToolbar), login_->widget());
    gtk_stack_add_named(GTK_STACK(stack_), loginToolbar, "login");

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window_), stack_);

    connection_ = session_.state().changed.connect([this](const SessionState& s) { onSessionChanged(s); });
    onSessionChanged(session_.state().get());
}

MainWindow::~MainWindow() {
    session_.state().changed.disconnect(connection_);
    app_.reset();
}

void MainWindow::onSessionChanged(const SessionState& state) {
    using Phase = SessionState::Phase;
    if (state.phase != Phase::Ready && app_) {
        // Drop every view-model right away; the session destroys the backend on its next turn.
        gtk_stack_set_visible_child_name(GTK_STACK(stack_), "login");
        GtkWidget* old = app_->widget();
        app_.reset();
        gtk_stack_remove(GTK_STACK(stack_), old);
    }
    switch (state.phase) {
        case Phase::Starting:
            gtk_stack_set_visible_child_name(GTK_STACK(stack_), "starting");
            break;
        case Phase::SignedOut:
        case Phase::WaitingForBrowser:
            login_->update(state);
            gtk_stack_set_visible_child_name(GTK_STACK(stack_), "login");
            break;
        case Phase::Ready:
            if (!app_ && session_.model()) {
                app_ = std::make_unique<AppPage>(*session_.model(), dataDir_,
                                                 [this] { session_.logout(); });
                gtk_stack_add_named(GTK_STACK(stack_), app_->widget(), "app");
                gtk_stack_set_visible_child_name(GTK_STACK(stack_), "app");
            }
            break;
    }
}

}  // namespace cardinal::gtk
