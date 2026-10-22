#include "pch.h"

#include "app_page.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "board_view.h"
#include "card_panel.h"
#include "style.h"
#include "win_util.h"

namespace cardinal::win {
namespace {

using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

const char* actionText(const std::string& kind) {
    if (kind == "move") return "mover o card";
    if (kind == "create") return "criar o card";
    if (kind == "edit") return "salvar a edição";
    if (kind == "label") return "adicionar a etiqueta";
    if (kind == "member") return "adicionar o membro";
    if (kind == "comment") return "enviar o comentário";
    return "salvar a alteração";
}

UIElement statusPage(const std::string& title, const std::string& description, bool spinner) {
    auto page = vstack(12);
    page.HorizontalAlignment(HorizontalAlignment::Center);
    page.VerticalAlignment(VerticalAlignment::Center);
    if (spinner) {
        ProgressRing ring;
        ring.IsActive(true);
        page.Children().Append(ring);
    }
    auto t = text(title, 24);
    t.HorizontalAlignment(HorizontalAlignment::Center);
    page.Children().Append(t);
    if (!description.empty()) {
        auto d = text(description);
        d.Opacity(0.7);
        d.HorizontalAlignment(HorizontalAlignment::Center);
        page.Children().Append(d);
    }
    return page;
}

}  // namespace

AppPage::AppPage(AppModel& model, std::string dataDir, std::function<void()> onLogout,
                 std::string openCardOnStart)
    : model_(model),
      dataDir_(std::move(dataDir)),
      onLogout_(std::move(onLogout)),
      openCardOnStart_(std::move(openCardOnStart)) {
    for (auto height : {GridLengthHelper::Auto(), GridLengthHelper::Auto(),
                        GridLengthHelper::FromValueAndType(1, GridUnitType::Star)}) {
        RowDefinition r;
        r.Height(height);
        root_.RowDefinitions().Append(r);
    }

    // Header: sync status on the left, the board switcher in the middle, the menu on the right.
    Grid header;
    header.Padding(ThicknessHelper::FromLengths(12, 6, 12, 6));
    for (auto width : {GridLengthHelper::FromValueAndType(1, GridUnitType::Star), GridLengthHelper::Auto(),
                       GridLengthHelper::FromValueAndType(1, GridUnitType::Star)}) {
        ColumnDefinition c;
        c.Width(width);
        header.ColumnDefinitions().Append(c);
    }

    auto status = hstack(8);
    status.VerticalAlignment(VerticalAlignment::Center);
    offlineIcon_.Glyph(L"");  // Warning
    offlineIcon_.Foreground(themeBrush(L"SystemFillColorCautionBrush"));
    accessibleName(offlineIcon_, "Sem conexão");
    show(offlineIcon_, false);
    sending_.Width(16);
    sending_.Height(16);
    sending_.IsActive(false);
    accessibleName(sending_, "Enviando alterações ao Trello");
    show(sending_, false);
    status.Children().Append(offlineIcon_);
    status.Children().Append(sending_);
    header.Children().Append(status);

    boardTitle_.Text(L"Boards");
    boardTitle_.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
    boardButton_.Content(boardTitle_);
    boardList_.SelectionMode(ListViewSelectionMode::None);
    boardList_.IsItemClickEnabled(true);
    boardList_.MaxHeight(420);
    boardList_.Width(280);
    boardList_.ItemClick([this](auto&&, ItemClickEventArgs const& e) {
        boardFlyout_.Hide();
        if (auto id = clickedId(e.ClickedItem()); !id.empty()) openBoard(id);
    });
    boardFlyout_.Content(boardList_);
    boardFlyout_.Placement(Primitives::FlyoutPlacementMode::Bottom);
    boardButton_.Flyout(boardFlyout_);
    Grid::SetColumn(boardButton_, 1);
    header.Children().Append(boardButton_);

    Button menu;
    SymbolIcon more(Symbol::More);
    menu.Content(more);
    menu.HorizontalAlignment(HorizontalAlignment::Right);
    ToolTipService::SetToolTip(menu, winrt::box_value(L"Menu"));
    accessibleName(menu, "Menu");
    MenuFlyout menuFlyout;
    MenuFlyoutItem logout;
    logout.Text(L"Sair da conta");
    logout.Click([this](auto&&, auto&&) { onLogout_(); });
    menuFlyout.Items().Append(logout);
    menu.Flyout(menuFlyout);
    Grid::SetColumn(menu, 2);
    header.Children().Append(menu);
    root_.Children().Append(header);

    offlineBanner_.Severity(InfoBarSeverity::Warning);
    offlineBanner_.IsClosable(false);
    offlineBanner_.Message(L"Sem conexão com o Trello. Suas alterações serão enviadas quando a conexão voltar.");
    Grid::SetRow(offlineBanner_, 1);
    root_.Children().Append(offlineBanner_);

    split_.PanePlacement(SplitViewPanePlacement::Right);
    split_.DisplayMode(SplitViewDisplayMode::Inline);
    split_.OpenPaneLength(420);
    split_.IsPaneOpen(false);
    split_.Content(statusPage("Carregando boards…", "", true));
    Grid::SetRow(split_, 2);
    root_.Children().Append(split_);

    // Rejected writes: a toast over the bottom of the board for six seconds.
    toast_.Severity(InfoBarSeverity::Error);
    toast_.IsClosable(true);
    toast_.HorizontalAlignment(HorizontalAlignment::Center);
    toast_.VerticalAlignment(VerticalAlignment::Bottom);
    toast_.Margin(ThicknessHelper::FromUniformLength(24));
    toast_.MaxWidth(560);
    Grid::SetRow(toast_, 2);
    root_.Children().Append(toast_);

    picker_ = model_.boardPicker();
    pickerConn_ = picker_->state().changed.connect([this](const BoardPickerState& s) { onPickerChanged(s); });
    statusConn_ = model_.status().changed.connect([this](const AppStatus& s) { onStatusChanged(s); });
    onStatusChanged(model_.status().get());
    model_.start();
}

AppPage::~AppPage() {
    if (toastTimer_) toastTimer_.Stop();
    picker_->state().changed.disconnect(pickerConn_);
    model_.status().changed.disconnect(statusConn_);
    if (boardVm_ && headerConn_) boardVm_->header().changed.disconnect(headerConn_);
    panel_.reset();
    board_.reset();
}

void AppPage::showToast(const std::string& message) {
    toast_.Message(hs(message));
    toast_.IsOpen(true);
    if (!toastTimer_) {
        toastTimer_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
        toastTimer_.IsRepeating(false);
        toastTimer_.Tick([this](auto&&, auto&&) { toast_.IsOpen(false); });
    }
    toastTimer_.Interval(std::chrono::seconds(6));
    toastTimer_.Start();
}

void AppPage::onStatusChanged(const AppStatus& s) {
    if (s.rejections > seenRejections_) {
        seenRejections_ = s.rejections;
        showToast(std::string("O Trello não aceitou: ") + actionText(s.lastRejectionKind) +
                  ". A alteração foi desfeita.");
    }
    offlineBanner_.IsOpen(s.offline);
    show(offlineBanner_, s.offline);
    show(offlineIcon_, s.offline);
    if (s.pendingWrites > 0) {
        std::string tip = std::to_string(s.pendingWrites) + (s.offline ? " alteração(ões) aguardando a conexão"
                                                                     : " alteração(ões) sendo enviadas…");
        ToolTipService::SetToolTip(s.offline ? UIElement(offlineIcon_) : UIElement(sending_),
                                   winrt::box_value(hs(tip)));
    }
    const bool sendingNow = s.pendingWrites > 0 && !s.offline;
    show(sending_, sendingNow);
    sending_.IsActive(sendingNow);
}

void AppPage::onPickerChanged(const BoardPickerState& state) {
    boardIds_.clear();
    boardList_.Items().Clear();
    for (const auto& b : state.boards) {
        boardIds_.push_back(b.id);
        ListViewItem item;
        auto name = text(b.name);
        name.Tag(winrt::box_value(hs(b.id)));
        item.Content(name);
        accessibleName(item, b.name);
        boardList_.Items().Append(item);
    }
    const bool known = std::find(boardIds_.begin(), boardIds_.end(), currentBoardId_) != boardIds_.end();
    if (!state.boards.empty() && !known) {
        std::string want = lastBoard();
        if (std::find(boardIds_.begin(), boardIds_.end(), want) == boardIds_.end()) want = boardIds_.front();
        openBoard(want);
    } else if (state.boards.empty() && !state.loading && currentBoardId_.empty()) {
        setContent(statusPage("Nenhum board encontrado",
                              "Crie ou entre num board no Trello e ele aparece aqui.", false));
    }
}

void AppPage::setContent(UIElement const& content) { split_.Content(content); }

std::string AppPage::lastBoard() const {
    std::ifstream in(std::filesystem::path(widen(dataDir_)) / "last-board");
    std::string id;
    std::getline(in, id);
    return id;
}

void AppPage::rememberBoard(const std::string& id) const {
    std::ofstream out(std::filesystem::path(widen(dataDir_)) / "last-board", std::ios::trunc);
    out << id << "\n";
}

void AppPage::openBoard(const std::string& boardId) {
    closeCard();
    if (boardVm_ && headerConn_) boardVm_->header().changed.disconnect(headerConn_);
    currentBoardId_ = boardId;
    rememberBoard(boardId);

    board_.reset();  // the old view goes before its view-model
    boardVm_ = model_.openBoard(boardId);
    board_ = std::make_unique<BoardView>(boardVm_, [this](const std::string& id) { openCard(id); });
    setContent(board_->widget());

    auto setTitle = [this](const BoardHeader& h) { boardTitle_.Text(hs(h.name.empty() ? "Board" : h.name)); };
    headerConn_ = boardVm_->header().changed.connect(setTitle);
    setTitle(boardVm_->header().get());
    if (!openCardOnStart_.empty()) {
        openCard(openCardOnStart_);
        openCardOnStart_.clear();
    }
}

void AppPage::openCard(const std::string& cardId) {
    if (board_) board_->saveScroll();
    panel_.reset();
    panel_ = std::make_unique<CardPanel>(model_.openCard(cardId), [this] { closeCard(); });
    split_.Pane(panel_->widget());
    split_.IsPaneOpen(true);
}

void AppPage::closeCard() {
    if (board_) board_->restoreScroll();
    split_.IsPaneOpen(false);
    if (!panel_) return;
    // May be called from the panel's own button: free it once the handler has returned.
    auto doomed = std::make_shared<std::unique_ptr<CardPanel>>(std::move(panel_));
    winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().TryEnqueue([doomed] { doomed->reset(); });
}

}  // namespace cardinal::win
