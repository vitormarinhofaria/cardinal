#include "app_page.h"

#include <algorithm>
#include <fstream>

#include "board_view.h"
#include "card_panel.h"

namespace cardinal::gtk {

AppPage::AppPage(AppModel& model, std::string dataDir, std::function<void()> onLogout)
    : model_(model), dataDir_(std::move(dataDir)), onLogout_(std::move(onLogout)) {
    root_ = adw_toolbar_view_new();

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

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(root_), header);
    offlineBanner_ = adw_banner_new("Sem conexão com o Trello. Suas alterações serão enviadas quando a conexão voltar.");
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(root_), offlineBanner_);

    GtkWidget* loading = adw_status_page_new();
    GtkWidget* spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_size_request(spinner, 32, 32);
    adw_status_page_set_child(ADW_STATUS_PAGE(loading), spinner);
    adw_status_page_set_title(ADW_STATUS_PAGE(loading), "Carregando boards…");
    content_ = loading;

    split_ = adw_overlay_split_view_new();
    adw_overlay_split_view_set_sidebar_position(ADW_OVERLAY_SPLIT_VIEW(split_), GTK_PACK_END);
    adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_), FALSE);
    adw_overlay_split_view_set_min_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(split_), 380);
    adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(split_), 560);
    adw_overlay_split_view_set_sidebar_width_fraction(ADW_OVERLAY_SPLIT_VIEW(split_), 0.32);
    adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split_), content_);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(root_), split_);

    picker_ = model_.boardPicker();
    pickerConn_ = picker_->state().changed.connect([this](const BoardPickerState& s) { onPickerChanged(s); });
    statusConn_ = model_.status().changed.connect([this](const AppStatus& s) { onStatusChanged(s); });
    onStatusChanged(model_.status().get());
    model_.start();
}

AppPage::~AppPage() {
    picker_->state().changed.disconnect(pickerConn_);
    model_.status().changed.disconnect(statusConn_);
    if (boardVm_ && headerConn_) boardVm_->header().changed.disconnect(headerConn_);
    panel_.reset();
    board_.reset();
}

void AppPage::onStatusChanged(const AppStatus& s) {
    adw_banner_set_revealed(ADW_BANNER(offlineBanner_), s.offline);
    gtk_widget_set_visible(syncIcon_, s.offline);
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
        adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split_), content_);
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
    board_ = std::make_unique<BoardView>(boardVm_, [this](const std::string& id) { openCard(id); });
    content_ = board_->widget();
    adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split_), content_);

    auto setTitle = [this](const BoardHeader& h) {
        gtk_menu_button_set_label(GTK_MENU_BUTTON(boardButton_), h.name.empty() ? "Board" : h.name.c_str());
    };
    headerConn_ = boardVm_->header().changed.connect(setTitle);
    setTitle(boardVm_->header().get());
}

void AppPage::openCard(const std::string& cardId) {
    auto vm = model_.openCard(cardId);
    panel_ = std::make_unique<CardPanel>(vm, [this] { closeCard(); });
    adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_), panel_->widget());
    adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_), TRUE);
}

void AppPage::closeCard() {
    adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_), FALSE);
    if (!panel_) return;
    // May be called from the panel's own button: free it once the handler has returned.
    auto* doomed = new std::unique_ptr<CardPanel>(std::move(panel_));
    g_idle_add_once(
        [](gpointer data) { delete static_cast<std::unique_ptr<CardPanel>*>(data); }, doomed);
}

}  // namespace cardinal::gtk
