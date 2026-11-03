#pragma once

#include <memory>
#include <string>

#include "cardinal/image_cache.h"
#include "gtk_util.h"

namespace cardinal::gtk {

// Pictures for the card: fetched through the disk cache, decoded already scaled down on a worker
// thread, and kept as textures in a small in-memory LRU so reopening a card shows them at once.
class ImageLoader {
public:
    explicit ImageLoader(ImageCache* cache);  // null: nothing loads (placeholders only)
    ~ImageLoader();

    // A square, center-cropped thumbnail `side` px wide, as a clickable button. `fullUrl` and
    // `title` are what the viewer shows when it is clicked.
    GtkWidget* thumbnail(const std::string& url, int side, const std::string& fullUrl,
                         const std::string& title);
    // The whole picture, fitting in `maxPx`, into `picture`. `onFail` runs on the UI thread if it
    // could not be loaded (not if the widget is gone by then).
    void loadInto(GtkPicture* picture, const std::string& url, int maxPx, Fn onFail = {});

    // A dialog over `parent` with the picture as large as fits.
    void showViewer(GtkWidget* parent, const std::string& url, const std::string& title);

    struct State;

private:
    std::shared_ptr<State> state_;
};

}  // namespace cardinal::gtk
