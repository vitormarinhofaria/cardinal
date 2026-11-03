#include "app_page.h"

#include <algorithm>
#include <fstream>

#include "board_view.h"
#include "card_panel.h"

namespace cardinal::gtk {

AppPage::AppPage(AppModel& model, ImageCache* images, std::string dataDir, std::function<void()> onLogout,
                 std::string openCardOnStart)
    : model_(model),
      imageCache_(images),
      images_(images),
      dataDir_(std::move(dataDir)),
      onLogout_(std::move(onLogout)),
      openCardOnStart_(std::move(openCardOnStart)) {
    toolbar_ = adw_toolbar_view_new();
    root_ = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(root_), toolbar_);

    GtkWidget* header = adw_header_bar_new();

    boardButton_ = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(boardButton_), "Boards");
    gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(boardButton_), TRUE);
    addClass(boardButton_, "flat");
    boardPopover_ = gtk_popover_new();
    boardList_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(boardList_), GTK_SELECTION_NONE);
    addClass(boardList_, "navigation-sidebar");
    GtkWidget* boardScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(boardScroll), boardList_);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(boardScroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(boardScroll), 420);
    gtk_widget_set_size_request(boardScroll, 280, -1);
    gtk_popover_set_child(GTK_POPOVER(boardPopover_), boardScroll);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(boardButton_), boardPopover_);
    g_signal_connect_data(
        boardList_, "row-activated",
        G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
            auto* self = static_cast<AppPage*>(data);
            size_t i = static_cast<size_t>(gtk_list_box_row_get_index(row));
            gtk_popover_popdown(GTK_POPOVER(self->boardPopover_));
            if (i < self->boardIds_.size()) self->openBoard(self->boardIds_[i]);
        }),
        this, nullptr, G_CONNECT_DEFAULT);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), boardButton_);

    syncIcon_ = gtk_image_new_from_icon_name("network-offline-symbolic");
    addClass(syncIcon_, "status-offline");
    gtk_widget_set_visible(syncIcon_, FALSE);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), syncIcon_);
    sendingSpinner_ = gtk_spinner_new();
    gtk_widget_set_tooltip_text(sendingSpinner_, "Enviando alterações ao Trello…");
    gtk_widget_set_visible(sendingSpinner_, FALSE);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), sendingSpinner_);

    GtkWidget* menuButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menuButton), "open-menu-symbolic");
    GtkWidget* menuPopover = gtk_popover_new();
    GtkWidget* logout = gtk_button_new_with_label("Sair da conta");
    addClass(logout, "flat");
    on(logout, "clicked", [this, menuPopover] {
        gtk_popover_popdown(GTK_POPOVER(menuPopover));
        onLogout_();
    });
    gtk_popover_set_child(GTK_POPOVER(menuPopover), logout);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menuButton), menuPopover);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menuButton);

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_), header);
    offlineBanner_ = adw_banner_new("Sem conexão com o Trello. Suas alterações serão enviadas quando a conexão voltar.");
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_), offlineBanner_);

    GtkWidget* loading = adw_status_page_new();
    GtkWidget* spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_size_request(spinner, 32, 32);
    adw_status_page_set_child(ADW_STATUS_PAGE(loading), spinner);
    adw_status_page_set_title(ADW_STATUS_PAGE(loading), "Carregando boards…");
    content_ = loading;

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_), content_);

    picker_ = model_.boardPicker();
    pickerConn_ = picker_->state().changed.connect([this](const BoardPickerState& s) { onPickerChanged(s); });
    statusConn_ = model_.status().changed.connect([this](const AppStatus& s) { onStatusChanged(s); });
    onStatusChanged(model_.status().get());
    model_.start();
}

AppPage::~AppPage() {
    *alive_ = false;
    picker_->state().changed.disconnect(pickerConn_);
    model_.status().changed.disconnect(statusConn_);
    if (boardVm_ && headerConn_) boardVm_->header().changed.disconnect(headerConn_);
    if (dialog_) {
        g_signal_handlers_disconnect_by_data(dialog_, this);
        adw_dialog_force_close(ADW_DIALOG(dialog_));
        dialog_ = nullptr;
    }
    panel_.reset();
    board_.reset();
}

namespace {
const char* actionText(const std::string& kind) {
    if (kind == "move") return "mover o card";
    if (kind == "create") return "criar o card";
    if (kind == "edit") return "salvar a edição";
    if (kind == "label") return "adicionar a etiqueta";
    if (kind == "member") return "adicionar o membro";
    if (kind == "comment") return "enviar o comentário";
    return "salvar a alteração";
}
}  // namespace

void AppPage::onStatusChanged(const AppStatus& s) {
    if (s.rejections > seenRejections_) {
        seenRejections_ = s.rejections;
        std::string text = std::string("O Trello não aceitou: ") + actionText(s.lastRejectionKind) + ". A alteração foi desfeita.";
        AdwToast* toast = adw_toast_new(text.c_str());
        adw_toast_set_timeout(toast, 6);
        adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(root_), toast);
    }
    adw_banner_set_revealed(ADW_BANNER(offlineBanner_), s.offline);
    gtk_widget_set_visible(syncIcon_, s.offline);
    if (s.pendingWrites > 0) {
        std::string tip = std::to_string(s.pendingWrites) + (s.offline ? " alteração(ões) aguardando a conexão"
                                                                     : " alteração(ões) sendo enviadas…");
        gtk_widget_set_tooltip_text(s.offline ? syncIcon_ : sendingSpinner_, tip.c_str());
    }
    const bool sending = s.pendingWrites > 0 && !s.offline;
    gtk_widget_set_visible(sendingSpinner_, sending);
    gtk_spinner_set_spinning(GTK_SPINNER(sendingSpinner_), sending);
}

void AppPage::onPickerChanged(const BoardPickerState& state) {
    boardIds_.clear();
    while (GtkWidget* c = gtk_widget_get_first_child(boardList_)) gtk_list_box_remove(GTK_LIST_BOX(boardList_), c);
    for (const auto& b : state.boards) {
        boardIds_.push_back(b.id);
        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* l = label(b.name);
        gtk_widget_set_margin_top(l, 8);
        gtk_widget_set_margin_bottom(l, 8);
        gtk_widget_set_margin_start(l, 8);
        gtk_widget_set_margin_end(l, 8);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), l);
        gtk_list_box_append(GTK_LIST_BOX(boardList_), row);
    }
    const bool known = std::find(boardIds_.begin(), boardIds_.end(), currentBoardId_) != boardIds_.end();
    if (!state.boards.empty() && !known) {
        std::string want = lastBoard();
        if (std::find(boardIds_.begin(), boardIds_.end(), want) == boardIds_.end()) want = boardIds_.front();
        openBoard(want);
    } else if (state.boards.empty() && !state.loading && currentBoardId_.empty()) {
        GtkWidget* empty = adw_status_page_new();
        adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "view-grid-symbolic");
        adw_status_page_set_title(ADW_STATUS_PAGE(empty), "Nenhum board encontrado");
        adw_status_page_set_description(ADW_STATUS_PAGE(empty), "Crie ou entre num board no Trello e ele aparece aqui.");
        content_ = empty;
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_), content_);
    }
}

std::string AppPage::lastBoard() const {
    std::ifstream in(dataDir_ + "/last-board");
    std::string id;
    std::getline(in, id);
    return id;
}

void AppPage::rememberBoard(const std::string& id) const {
    std::ofstream out(dataDir_ + "/last-board", std::ios::trunc);
    out << id << "\n";
}

void AppPage::openBoard(const std::string& boardId) {
    closeCard();
    if (boardVm_ && headerConn_) boardVm_->header().changed.disconnect(headerConn_);
    currentBoardId_ = boardId;
    rememberBoard(boardId);

    boardVm_ = model_.openBoard(boardId);
    board_ = std::make_unique<BoardView>(boardVm_, imageCache_, [this](const std::string& id) { openCard(id); });
    content_ = board_->widget();
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_), content_);

    auto setTitle = [this](const BoardHeader& h) {
        gtk_menu_button_set_label(GTK_MENU_BUTTON(boardButton_), h.name.empty() ? "Board" : h.name.c_str());
    };
    headerConn_ = boardVm_->header().changed.connect(setTitle);
    setTitle(boardVm_->header().get());
    if (const char* mode = g_getenv("CARDINAL_SELFTEST"); mode && std::string(mode) == "move") {
        // Development aid: move card c2 to the top of the second list, as a drag would.
        g_timeout_add(900, [](gpointer d) -> gboolean {
            auto* self = static_cast<AppPage*>(d);
            if (self->boardVm_ && self->boardVm_->columns().size() > 1)
                self->boardVm_->moveCard("c2", self->boardVm_->columns()[1]->id, 0);
            return G_SOURCE_REMOVE;
        }, this);
    }
    if (!openCardOnStart_.empty()) {
        openCard(openCardOnStart_);
        openCardOnStart_.clear();
    }
}

void AppPage::openCard(const std::string& cardId) {
    trace("panel: open requested");
    if (dialog_) {  // only one card at a time
        g_signal_handlers_disconnect_by_data(dialog_, this);
        adw_dialog_force_close(ADW_DIALOG(dialog_));
        onDialogClosed();
    }
    auto vm = model_.openCard(cardId);
    // A link to another card opens it in place of this one, once the click's handlers are done.
    auto openLinked = [this](const std::string& id) {
        struct Pending {
            AppPage* page;
            std::shared_ptr<bool> alive;
            std::string cardId;
        };
        g_idle_add_once([](gpointer d) {
            std::unique_ptr<Pending> p(static_cast<Pending*>(d));
            if (*p->alive) p->page->openCard(p->cardId);
        }, new Pending{this, alive_, id});
    };
    panel_ = std::make_unique<CardPanel>(vm, &images_, [this] { closeCard(); }, openLinked);

    dialog_ = GTK_WIDGET(adw_dialog_new());
    adw_dialog_set_child(ADW_DIALOG(dialog_), panel_->widget());
    adw_dialog_set_content_width(ADW_DIALOG(dialog_), 960);
    adw_dialog_set_content_height(ADW_DIALOG(dialog_), 720);
    gtk_widget_set_size_request(dialog_, 360, 320);  // breakpoints need a minimum size
    // Narrow window: one column, labels and members first.
    AdwBreakpoint* bp = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 560sp"));
    g_signal_connect_data(bp, "apply", G_CALLBACK(+[](AdwBreakpoint*, gpointer d) {
        auto* self = static_cast<AppPage*>(d);
        if (self->panel_) self->panel_->setNarrow(true);
    }), this, nullptr, G_CONNECT_DEFAULT);
    g_signal_connect_data(bp, "unapply", G_CALLBACK(+[](AdwBreakpoint*, gpointer d) {
        auto* self = static_cast<AppPage*>(d);
        if (self->panel_) self->panel_->setNarrow(false);
    }), this, nullptr, G_CONNECT_DEFAULT);
    adw_dialog_add_breakpoint(ADW_DIALOG(dialog_), bp);
    g_signal_connect_data(dialog_, "closed", G_CALLBACK(+[](AdwDialog*, gpointer d) {
        static_cast<AppPage*>(d)->onDialogClosed();
    }), this, nullptr, G_CONNECT_DEFAULT);
    // Esc closes the card. Handled before the children see it: the title (which gets the focus)
    // swallows Esc even when it is not being edited. While editing it, Esc only cancels the edit.
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect_data(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint,
                                                              GdkModifierType, gpointer d) -> gboolean {
        auto* self = static_cast<AppPage*>(d);
        if (keyval != GDK_KEY_Escape || !self->panel_ || self->panel_->editingTitle()) return FALSE;
        self->closeCard();
        return TRUE;
    }), this, nullptr, G_CONNECT_DEFAULT);
    gtk_widget_add_controller(dialog_, keys);

    // A click on the dimmed board around the card closes it, like on Trello. Both press and release
    // must land outside, so a text selection dragged past the edge does not close it.
    GtkGesture* outside = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(outside), GDK_BUTTON_PRIMARY);
    // Capture: the backdrop is a window handle whose own gesture would otherwise see it first.
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(outside), GTK_PHASE_CAPTURE);
    g_signal_connect_data(outside, "pressed", G_CALLBACK(+[](GtkGestureClick*, int, double x, double y,
                                                             gpointer d) {
        auto* self = static_cast<AppPage*>(d);
        self->pressedOutside_ = self->outsideCard(x, y);
    }), this, nullptr, G_CONNECT_DEFAULT);
    g_signal_connect_data(outside, "released", G_CALLBACK(+[](GtkGestureClick*, int, double x, double y,
                                                              gpointer d) {
        auto* self = static_cast<AppPage*>(d);
        if (self->pressedOutside_ && self->outsideCard(x, y)) self->closeCard();
        self->pressedOutside_ = false;
    }), this, nullptr, G_CONNECT_DEFAULT);
    gtk_widget_add_controller(dialog_, GTK_EVENT_CONTROLLER(outside));

    adw_dialog_present(ADW_DIALOG(dialog_), root_);

    if (g_getenv("CARDINAL_SELFTEST")) {
        g_timeout_add(700, [](gpointer d) -> gboolean {
            const char* mode = g_getenv("CARDINAL_SELFTEST");
            if (mode && std::string(mode) == "draft")
                static_cast<CardPanel*>(d)->selfTestDraft();
            else
                static_cast<CardPanel*>(d)->selfTest();
            return G_SOURCE_REMOVE;
        }, panel_.get());
    }
}

void AppPage::closeCard() {
    if (dialog_) adw_dialog_close(ADW_DIALOG(dialog_));  // "closed" frees the panel
}

bool AppPage::outsideCard(double x, double y) const {
    if (!dialog_ || !panel_) return false;
    graphene_point_t in = GRAPHENE_POINT_INIT(static_cast<float>(x), static_cast<float>(y)), out;
    if (!gtk_widget_compute_point(dialog_, panel_->widget(), &in, &out)) return false;
    return !gtk_widget_contains(panel_->widget(), out.x, out.y);
}

void AppPage::onDialogClosed() {
    dialog_ = nullptr;
    if (!panel_) return;
    // May be called from inside the panel's own handlers: free it once they have returned.
    auto* doomed = new std::unique_ptr<CardPanel>(std::move(panel_));
    g_idle_add_once(
        [](gpointer data) { delete static_cast<std::unique_ptr<CardPanel>*>(data); }, doomed);
}

}  // namespace cardinal::gtk
