#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cardinal/viewmodel.h"
#include "gtk_util.h"

namespace cardinal::gtk {

// Trello-style board: a horizontal row of columns, each a virtualized list of cards. Pure view:
// everything comes from BoardVM, every action goes back to it.
class BoardView {
public:
    BoardView(std::shared_ptr<BoardVM> vm, std::function<void(const std::string&)> onOpenCard);
    ~BoardView();
    GtkWidget* widget() const { return root_; }

    struct Column;  // widgets of one column; owned by its root widget

    // Called by GTK callbacks.
    void openCard(const std::string& cardId) { onOpenCard_(cardId); }
    void dropCard(const std::string& cardId, const std::string& toListId, int index);

private:
    void rebuildStructure();
    void markVisibilityDirty();
    void attachVisibleColumns();
    void updateColumn(const std::string& listId);
    Column* ensureColumn(const std::string& listId);

    std::shared_ptr<BoardVM> vm_;
    std::function<void(const std::string&)> onOpenCard_;
    int structureConn_ = 0;
    int columnConn_ = 0;
    GtkWidget* root_;
    GtkWidget* box_;
    std::map<std::string, Column*> columns_;  // raw: each Column dies with its root widget
    std::vector<std::string> order_;
    bool visibilityDirty_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace cardinal::gtk
