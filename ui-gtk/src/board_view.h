#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cardinal/image_cache.h"
#include "cardinal/viewmodel.h"
#include "gtk_util.h"

namespace cardinal::gtk {

// Trello-style board: a horizontal row of columns, each a virtualized list of cards. Pure view:
// everything comes from BoardVM, every action goes back to it.
class BoardView {
public:
    // `images`: where the board's background picture is fetched; null shows its color only.
    BoardView(std::shared_ptr<BoardVM> vm, ImageCache* images,
              std::function<void(const std::string&)> onOpenCard);
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
    void fitColumns();
    int fitHeight_ = 0;
    bool panning_ = false;
    double panStart_ = 0;
    void updateColumn(const std::string& listId);
    Column* ensureColumn(const std::string& listId);
    void applyBackground(const BoardBackground& bg);
    void setBackgroundCss(const std::string& css);

    std::shared_ptr<BoardVM> vm_;
    std::function<void(const std::string&)> onOpenCard_;
    int structureConn_ = 0;
    int columnConn_ = 0;
    int headerConn_ = 0;
    gulong darkConn_ = 0;  // AdwStyleManager::notify::dark
    ImageCache* images_ = nullptr;
    GtkCssProvider* bgProvider_ = nullptr;  // this board's background, on the default display
    std::string bgClass_;                    // unique class tying bgProvider_ to root_
    BoardBackground bgShown_;
    std::string bgPicture_;                  // local file of the picture being shown
    GtkWidget* root_;
    GtkWidget* box_;
    std::map<std::string, Column*> columns_;  // raw: each Column dies with its root widget
    std::vector<std::string> order_;
    bool visibilityDirty_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace cardinal::gtk
