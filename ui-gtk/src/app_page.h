#pragma once

#include <memory>
#include <string>

#include "cardinal/viewmodel.h"
#include "gtk_util.h"

namespace cardinal::gtk {

class BoardView;
class CardPanel;

// The signed-in screen: header with board switcher and sync status, the board, and the card panel
// as a side sheet. Holds every view-model, so destroying it releases them all.
class AppPage {
public:
    AppPage(AppModel& model, std::string dataDir, std::function<void()> onLogout,
            std::string openCardOnStart = "");
    ~AppPage();
    GtkWidget* widget() const { return root_; }

private:
    void onPickerChanged(const BoardPickerState& state);
    void onStatusChanged(const AppStatus& status);
    void openBoard(const std::string& boardId);
    void openCard(const std::string& cardId);
    void closeCard();
    std::string lastBoard() const;
    void rememberBoard(const std::string& id) const;

    AppModel& model_;
    std::string dataDir_;
    std::function<void()> onLogout_;

    std::shared_ptr<BoardPickerVM> picker_;
    std::shared_ptr<BoardVM> boardVm_;
    std::unique_ptr<BoardView> board_;
    std::unique_ptr<CardPanel> panel_;
    int pickerConn_ = 0;
    int statusConn_ = 0;
    int headerConn_ = 0;
    std::string currentBoardId_;
    std::string openCardOnStart_;
    std::vector<std::string> boardIds_;

    GtkWidget* root_;
    GtkWidget* boardButton_;
    GtkWidget* boardList_;
    GtkWidget* boardPopover_;
    GtkWidget* offlineBanner_;
    GtkWidget* syncIcon_;
    GtkWidget* sendingSpinner_;
    GtkWidget* split_;
    GtkWidget* content_;  // holds the board, or a loading page
    GtkWidget* sidebarHolder_;
};

}  // namespace cardinal::gtk
