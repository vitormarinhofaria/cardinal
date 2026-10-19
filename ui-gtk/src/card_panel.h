#pragma once

#include <functional>
#include <memory>

#include "cardinal/viewmodel.h"
#include "gtk_util.h"

namespace cardinal::gtk {

class CardPanel {
public:
    CardPanel(std::shared_ptr<CardPanelVM> vm, std::function<void()> onClose);
    ~CardPanel();
    GtkWidget* widget() const { return root_; }

private:
    void update(const CardPanelState& s);

    std::shared_ptr<CardPanelVM> vm_;
    std::function<void()> onClose_;
    int conn_ = 0;
    GtkWidget* root_;
    GtkWidget* title_;
};

}  // namespace cardinal::gtk
