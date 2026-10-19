#include "platform.h"

#include <gio/gio.h>
#include <glib.h>

#include <stdexcept>

namespace cardinal::gtk {

void GtkDispatcher::post(std::function<void()> fn) {
    auto* boxed = new std::function<void()>(std::move(fn));
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            auto* f = static_cast<std::function<void()>*>(data);
            (*f)();
            return G_SOURCE_REMOVE;
        },
        boxed, [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}

void GtkBrowserOpener::open(const std::string& url) {
    GError* error = nullptr;
    if (!g_app_info_launch_default_for_uri(url.c_str(), nullptr, &error)) {
        std::string msg = error ? error->message : "unknown error";
        if (error) g_error_free(error);
        throw std::runtime_error(msg);
    }
}

}  // namespace cardinal::gtk
