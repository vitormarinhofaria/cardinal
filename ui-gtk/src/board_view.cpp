#include "board_view.h"

namespace cardinal::gtk {

// Placeholder until the real board view lands (columns, virtualized cards, drag and drop).
BoardView::BoardView(std::shared_ptr<BoardVM> vm, std::function<void(const std::string&)> onOpenCard)
    : vm_(std::move(vm)), onOpenCard_(std::move(onOpenCard)) {
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(box_, 12);
    gtk_widget_set_margin_start(box_, 12);
    root_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(root_), box_);
    addClass(root_, "board-area");
    structureConn_ = -1;
    vm_->structureChanged.connect([this] { rebuild(); });
    rebuild();
}

BoardView::~BoardView() = default;

void BoardView::rebuild() {
    clearChildren(box_);
    for (const auto& c : vm_->columns()) {
        std::string text = c->name + " — " + std::to_string(c->cardCount) + " cards";
        GtkWidget* l = label(text, "heading");
        addClass(l, "card");
        gtk_box_append(GTK_BOX(box_), l);
    }
}

}  // namespace cardinal::gtk
