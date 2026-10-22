#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cardinal/viewmodel.h"

namespace cardinal::win {

// Trello-style board: a horizontal row of columns, each a virtualized ListView of cards. Pure
// view: everything comes from BoardVM, every action goes back to it.
class BoardView {
public:
    BoardView(std::shared_ptr<BoardVM> vm, std::function<void(const std::string&)> onOpenCard);
    ~BoardView();
    winrt::Microsoft::UI::Xaml::UIElement widget() const { return root_; }

    // Opening the side panel narrows the board and the ScrollViewer clamps its offset; remember
    // it on open and put it back (while the board widens again) on close.
    void saveScroll();
    void restoreScroll();

    struct Column;

private:
    void rebuildStructure();
    void updateColumn(const std::string& listId);
    Column& ensureColumn(const std::string& listId);
    void dropCard(const std::string& cardId, const std::string& toListId, int index);

    std::shared_ptr<BoardVM> vm_;
    std::function<void(const std::string&)> onOpenCard_;
    int structureConn_ = 0;
    int columnConn_ = 0;
    winrt::Microsoft::UI::Xaml::Controls::Grid root_;
    winrt::Microsoft::UI::Xaml::Controls::ScrollViewer scroller_;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel box_;
    std::map<std::string, std::unique_ptr<Column>> columns_;
    std::string dragging_;  // card id of the drag in progress, if it started on this board
    bool scrollSaved_ = false;
    bool restoring_ = false;
    double savedScroll_ = 0;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer restoreTimer_{nullptr};
};

}  // namespace cardinal::win
