#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cardinal/viewmodel.h"
#include "gtk_util.h"
#include "image_loader.h"

namespace cardinal::gtk {

struct MarkdownContext {
    ImageLoader* images = nullptr;  // null: pictures show as links
    // The URL to fetch for a picture (a smaller preview when there is one), or nullopt when it is
    // not known yet which one: a placeholder shows until the text is rendered again.
    std::function<std::optional<std::string>(const std::string& url)> pictureUrl;
    const std::vector<CardLink>* cards = nullptr;  // what the links to cards resolved to
    std::function<void(const std::string& cardId)> openCard;
    int pictureWidth = 480;   // largest picture width, px
    bool selectable = false;  // text can be selected (a click on it then does not reach the parent)
};

// The markdown `text` laid out as widgets: a vertical box of labels, pictures, code and tables.
// Web links open in the browser, links to cards call `openCard`; any other kind of link is ignored.
GtkWidget* renderMarkdown(std::string_view text, const MarkdownContext& ctx);

}  // namespace cardinal::gtk
