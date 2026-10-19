#include "card_panel.h"

namespace cardinal::gtk {

// Placeholder until the real panel lands.
CardPanel::CardPanel(std::shared_ptr<CardPanelVM> vm, std::function<void()> onClose)
    : vm_(std::move(vm)), onClose_(std::move(onClose)) {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    addClass(root_, "panel");
    title_ = label("", "panel-title");
    gtk_box_append(GTK_BOX(root_), title_);
    GtkWidget* close = gtk_button_new_with_label("Fechar");
    on(close, "clicked", [this] { onClose_(); });
    gtk_box_append(GTK_BOX(root_), close);
    conn_ = vm_->state().changed.connect([this](const CardPanelState& s) { update(s); });
    update(vm_->state().get());
}

CardPanel::~CardPanel() { vm_->state().changed.disconnect(conn_); }

void CardPanel::update(const CardPanelState& s) { gtk_label_set_text(GTK_LABEL(title_), s.title.c_str()); }

}  // namespace cardinal::gtk
