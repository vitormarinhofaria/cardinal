#include "login_page.h"

namespace cardinal::gtk {
namespace {

const char* errorText(SessionError e) {
    switch (e) {
        case SessionError::None: return "";
        case SessionError::MissingApiKey:
            return "Esta compilação não tem uma API key do Trello. Compile com "
                   "-DCARDINAL_TRELLO_API_KEY=… ou defina CARDINAL_API_KEY.";
        case SessionError::BrowserFailed: return "Não foi possível abrir o navegador.";
        case SessionError::LoginTimedOut: return "O login demorou demais. Tente de novo.";
        case SessionError::InvalidToken: return "Esse token não parece válido.";
        case SessionError::SessionExpired: return "Sua sessão expirou. Entre de novo para continuar.";
    }
    return "";
}

}  // namespace

LoginPage::LoginPage(Session& session) : session_(session) {
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_halign(content, GTK_ALIGN_CENTER);

    signIn_ = gtk_button_new_with_label("Entrar com o Trello");
    addClass(signIn_, "suggested-action");
    addClass(signIn_, "pill");
    gtk_widget_set_halign(signIn_, GTK_ALIGN_CENTER);
    on(signIn_, "clicked", [this] { session_.startLogin(); });
    gtk_box_append(GTK_BOX(content), signIn_);

    waiting_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget* spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_size_request(spinner, 32, 32);
    gtk_box_append(GTK_BOX(waiting_), spinner);
    gtk_box_append(GTK_BOX(waiting_), label("Aguardando você autorizar no navegador…", nullptr, 0.5f));
    GtkWidget* cancel = gtk_button_new_with_label("Cancelar");
    gtk_widget_set_halign(cancel, GTK_ALIGN_CENTER);
    on(cancel, "clicked", [this] { session_.cancelLogin(); });
    gtk_box_append(GTK_BOX(waiting_), cancel);
    gtk_box_append(GTK_BOX(content), waiting_);

    error_ = label("", "error", 0.5f);
    gtk_label_set_wrap(GTK_LABEL(error_), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(error_), 48);
    gtk_box_append(GTK_BOX(content), error_);

    GtkWidget* manual = gtk_expander_new("Colar um token manualmente");
    GtkWidget* manualBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    tokenEntry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(tokenEntry_), "Token do Trello");
    gtk_entry_set_visibility(GTK_ENTRY(tokenEntry_), FALSE);
    gtk_widget_set_hexpand(tokenEntry_, TRUE);
    GtkWidget* use = gtk_button_new_with_label("Usar token");
    auto submit = [this] {
        session_.loginWithToken(str(gtk_editable_get_text(GTK_EDITABLE(tokenEntry_))));
    };
    on(use, "clicked", submit);
    on(tokenEntry_, "activate", submit);
    gtk_box_append(GTK_BOX(manualBox), tokenEntry_);
    gtk_box_append(GTK_BOX(manualBox), use);
    gtk_expander_set_child(GTK_EXPANDER(manual), manualBox);
    gtk_box_append(GTK_BOX(content), manual);

    root_ = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(root_), "view-grid-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(root_), "Cardinal");
    adw_status_page_set_description(ADW_STATUS_PAGE(root_),
                                    "Um cliente rápido e nativo para o Trello.");
    adw_status_page_set_child(ADW_STATUS_PAGE(root_), content);
    update(session.state().get());
}

void LoginPage::update(const SessionState& state) {
    const bool waiting = state.phase == SessionState::Phase::WaitingForBrowser;
    gtk_widget_set_visible(signIn_, !waiting);
    gtk_widget_set_visible(waiting_, waiting);
    const char* text = errorText(state.error);
    gtk_label_set_text(GTK_LABEL(error_), text);
    gtk_widget_set_visible(error_, *text != '\0');
    if (!waiting) gtk_editable_set_text(GTK_EDITABLE(tokenEntry_), "");
}

}  // namespace cardinal::gtk
