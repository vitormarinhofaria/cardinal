#pragma once

#include <functional>
#include <memory>
#include <string>

#include "cardinal/viewmodel.h"
#include "gtk_util.h"

namespace cardinal::gtk {

class BoardView {
public:
    BoardView(std::shared_ptr<BoardVM> vm, std::function<void(const std::string&)> onOpenCard);
    ~BoardView();
    GtkWidget* widget() const { return root_; }

private:
    void rebuild();

    std::shared_ptr<BoardVM> vm_;
    std::function<void(const std::string&)> onOpenCard_;
    int structureConn_ = 0;
    GtkWidget* root_;
    GtkWidget* box_;
};

}  // namespace cardinal::gtk
