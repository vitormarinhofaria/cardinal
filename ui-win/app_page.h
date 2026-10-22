#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cardinal/viewmodel.h"

namespace cardinal::win {

class BoardView;
class CardPanel;

// The signed-in screen: header with board switcher and sync status, the board, and the card panel
// as a side pane. Holds every view-model, so destroying it releases them all.
class AppPage {
public:
    AppPage(AppModel& model, std::string dataDir, std::function<void()> onLogout,
            std::string openCardOnStart = "");
    ~AppPage();
    winrt::Microsoft::UI::Xaml::UIElement widget() const { return root_; }

private:
    void onPickerChanged(const BoardPickerState& state);
    void onStatusChanged(const AppStatus& status);
    void openBoard(const std::string& boardId);
    void openCard(const std::string& cardId);
    void closeCard();
    void setContent(winrt::Microsoft::UI::Xaml::UIElement const& content);
    void showToast(const std::string& message);
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
    int seenRejections_ = 0;

    winrt::Microsoft::UI::Xaml::Controls::Grid root_;
    winrt::Microsoft::UI::Xaml::Controls::DropDownButton boardButton_;
    winrt::Microsoft::UI::Xaml::Controls::TextBlock boardTitle_;
    winrt::Microsoft::UI::Xaml::Controls::Flyout boardFlyout_;
    winrt::Microsoft::UI::Xaml::Controls::ListView boardList_;
    winrt::Microsoft::UI::Xaml::Controls::FontIcon offlineIcon_;
    winrt::Microsoft::UI::Xaml::Controls::ProgressRing sending_;
    winrt::Microsoft::UI::Xaml::Controls::InfoBar offlineBanner_;
    winrt::Microsoft::UI::Xaml::Controls::InfoBar toast_;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer toastTimer_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::SplitView split_;
};

}  // namespace cardinal::win
