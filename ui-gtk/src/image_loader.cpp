#include "image_loader.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <vector>

namespace cardinal::gtk {
namespace {

enum class Fit { Square, Contain };

constexpr size_t kMemoryBudget = 96u * 1024 * 1024;  // decoded textures kept for reopening cards
constexpr double kMaxPixels = 80e6;                  // refuse absurd dimensions before decoding

std::string keyOf(const std::string& url, Fit fit, int px) {
    return url + (fit == Fit::Square ? "#sq" : "#fit") + std::to_string(px);
}

// Runs on a worker thread. Decodes at (close to) the size that will be shown: JPEG decodes at a
// reduced scale directly, which is the bulk of the speedup on photos.
GdkTexture* decode(const std::string& path, Fit fit, int px) {
    int w = 0, h = 0;
    GdkPixbufFormat* format = gdk_pixbuf_get_file_info(path.c_str(), &w, &h);
    if (!format || w <= 0 || h <= 0 || double(w) * h > kMaxPixels) return nullptr;
    gchar* name = gdk_pixbuf_format_get_name(format);
    const bool vector = name && (std::string(name) == "svg" || std::string(name) == "wmf");
    g_free(name);
    if (vector) return nullptr;  // only raster pictures; vector loaders are a larger attack surface

    const double scale = fit == Fit::Square ? double(px) / std::min(w, h) : double(px) / std::max(w, h);
    GError* error = nullptr;
    GdkPixbuf* loaded = scale < 1
        ? gdk_pixbuf_new_from_file_at_scale(path.c_str(), std::max(1, int(std::lround(w * scale))),
                                            std::max(1, int(std::lround(h * scale))), FALSE, &error)
        : gdk_pixbuf_new_from_file(path.c_str(), &error);
    if (!loaded) {
        g_clear_error(&error);
        return nullptr;
    }
    GdkPixbuf* pb = gdk_pixbuf_apply_embedded_orientation(loaded);  // phone photos
    g_object_unref(loaded);
    if (!pb) return nullptr;
    if (fit == Fit::Square) {
        const int pw = gdk_pixbuf_get_width(pb), ph = gdk_pixbuf_get_height(pb), side = std::min(pw, ph);
        GdkPixbuf* sub = gdk_pixbuf_new_subpixbuf(pb, (pw - side) / 2, (ph - side) / 2, side, side);
        GdkPixbuf* square = gdk_pixbuf_copy(sub);
        g_object_unref(sub);
        g_object_unref(pb);
        pb = square;
        if (!pb) return nullptr;
    }
    GBytes* bytes = gdk_pixbuf_read_pixel_bytes(pb);
    GdkTexture* texture = gdk_memory_texture_new(
        gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb),
        gdk_pixbuf_get_has_alpha(pb) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, bytes,
        static_cast<gsize>(gdk_pixbuf_get_rowstride(pb)));
    g_bytes_unref(bytes);
    g_object_unref(pb);
    return texture;
}

size_t sizeOf(GdkTexture* t) {
    return static_cast<size_t>(gdk_texture_get_width(t)) * gdk_texture_get_height(t) * 4;
}

}  // namespace

// UI thread only, except where noted.
struct ImageLoader::State {
    ImageCache* cache = nullptr;

    struct Target {
        GWeakRef widget;
        std::function<void(GtkWidget*, GdkTexture*)> apply;  // texture null: it failed
    };
    std::map<std::string, std::vector<std::unique_ptr<Target>>> pending;  // key -> who waits

    struct Entry {
        GdkTexture* texture;
        std::list<std::string>::iterator use;
    };
    std::map<std::string, Entry> memory;
    std::list<std::string> recent;  // front = most recently used
    size_t bytes = 0;

    ~State() {
        for (auto& [k, e] : memory) g_object_unref(e.texture);
        for (auto& [k, list] : pending)
            for (auto& t : list) g_weak_ref_clear(&t->widget);
    }

    GdkTexture* lookup(const std::string& key) {
        auto it = memory.find(key);
        if (it == memory.end()) return nullptr;
        recent.splice(recent.begin(), recent, it->second.use);
        return it->second.texture;
    }

    void remember(const std::string& key, GdkTexture* texture) {
        if (memory.count(key)) return;
        recent.push_front(key);
        memory[key] = {GDK_TEXTURE(g_object_ref(texture)), recent.begin()};
        bytes += sizeOf(texture);
        while (bytes > kMemoryBudget && recent.size() > 1) {
            auto victim = memory.find(recent.back());
            bytes -= sizeOf(victim->second.texture);
            g_object_unref(victim->second.texture);
            memory.erase(victim);
            recent.pop_back();
        }
    }

    void finish(const std::string& key, GdkTexture* texture) {
        if (texture) remember(key, texture);
        auto it = pending.find(key);
        if (it == pending.end()) return;
        auto targets = std::move(it->second);
        pending.erase(it);
        for (auto& t : targets) {
            if (GtkWidget* w = static_cast<GtkWidget*>(g_weak_ref_get(&t->widget))) {
                t->apply(w, texture);
                g_object_unref(w);
            }
            g_weak_ref_clear(&t->widget);
        }
    }
};

namespace {

void request(const std::shared_ptr<ImageLoader::State>& state, GtkWidget* widget, const std::string& url,
             Fit fit, int px, std::function<void(GtkWidget*, GdkTexture*)> apply) {
    const std::string key = keyOf(url, fit, px);
    if (GdkTexture* t = state->lookup(key)) {  // already decoded: no I/O at all
        apply(widget, t);
        return;
    }
    if (!state->cache) {
        apply(widget, nullptr);
        return;
    }
    auto target = std::make_unique<ImageLoader::State::Target>();
    g_weak_ref_init(&target->widget, widget);
    target->apply = std::move(apply);
    auto& waiting = state->pending[key];
    waiting.push_back(std::move(target));
    if (waiting.size() > 1) return;  // same picture, same size, already on its way

    std::weak_ptr<ImageLoader::State> weak = state;
    state->cache->fetch(url, [weak, key, fit, px](std::optional<std::filesystem::path> file) {
        // worker thread
        GdkTexture* texture = file ? decode(file->string(), fit, px) : nullptr;
        struct Result {
            std::weak_ptr<ImageLoader::State> state;
            std::string key;
            GdkTexture* texture;
        };
        g_main_context_invoke_full(
            nullptr, G_PRIORITY_DEFAULT,
            [](gpointer d) -> gboolean {
                auto* r = static_cast<Result*>(d);
                if (auto s = r->state.lock()) s->finish(r->key, r->texture);
                return G_SOURCE_REMOVE;
            },
            new Result{weak, key, texture},
            [](gpointer d) {
                auto* r = static_cast<Result*>(d);
                if (r->texture) g_object_unref(r->texture);
                delete r;
            });
    });
}

}  // namespace

ImageLoader::ImageLoader(ImageCache* cache) : state_(std::make_shared<State>()) { state_->cache = cache; }

ImageLoader::~ImageLoader() = default;

GtkWidget* ImageLoader::thumbnail(const std::string& url, int side, const std::string& fullUrl,
                                  const std::string& title) {
    GtkWidget* image = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(image), side);
    gtk_widget_set_size_request(image, side, side);
    gtk_widget_set_overflow(image, GTK_OVERFLOW_HIDDEN);
    addClass(image, "thumb");
    addClass(image, "thumb-loading");

    GtkWidget* button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(button), image);
    addClass(button, "flat");
    addClass(button, "thumb-button");
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    gtk_widget_set_valign(button, GTK_ALIGN_START);
    if (!title.empty()) gtk_widget_set_tooltip_text(button, title.c_str());
    on(button, "clicked", [this, button, fullUrl, title] { showViewer(button, fullUrl, title); });

    // Decoded at twice the size: sharp on HiDPI screens, GtkImage scales it down on the GPU.
    const int scale = std::max(2, gtk_widget_get_scale_factor(button));
    request(state_, image, url, Fit::Square, side * scale, [side](GtkWidget* w, GdkTexture* t) {
        gtk_widget_remove_css_class(w, "thumb-loading");
        if (t) {
            gtk_image_set_from_paintable(GTK_IMAGE(w), GDK_PAINTABLE(t));
        } else {
            gtk_image_set_from_icon_name(GTK_IMAGE(w), "image-missing-symbolic");
            gtk_image_set_pixel_size(GTK_IMAGE(w), std::min(side, 32));
            addClass(w, "thumb-failed");
            gtk_widget_set_tooltip_text(w, "Não foi possível carregar a imagem");
        }
    });
    return button;
}

void ImageLoader::loadInto(GtkPicture* picture, const std::string& url, int maxPx, Fn onFail) {
    request(state_, GTK_WIDGET(picture), url, Fit::Contain, maxPx,
            [onFail = std::move(onFail)](GtkWidget* w, GdkTexture* t) {
                if (t)
                    gtk_picture_set_paintable(GTK_PICTURE(w), GDK_PAINTABLE(t));
                else if (onFail)
                    onFail();
            });
}

void ImageLoader::showViewer(GtkWidget* parent, const std::string& url, const std::string& title) {
    AdwDialog* dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, title.empty() ? "Imagem" : title.c_str());
    adw_dialog_set_content_width(dialog, 1100);
    adw_dialog_set_content_height(dialog, 800);

    GtkWidget* header = adw_header_bar_new();
    const bool web = url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
    if (web) {
        GtkWidget* open = gtk_button_new_from_icon_name("external-link-symbolic");
        gtk_widget_set_tooltip_text(open, "Abrir no navegador");
        on(open, "clicked", [open, url] {
            GtkUriLauncher* launcher = gtk_uri_launcher_new(url.c_str());
            gtk_uri_launcher_launch(launcher, GTK_WINDOW(gtk_widget_get_root(open)), nullptr, nullptr, nullptr);
            g_object_unref(launcher);
        });
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), open);
    }

    GtkWidget* picture = gtk_picture_new();
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_SCALE_DOWN);
    gtk_widget_set_vexpand(picture, TRUE);
    gtk_widget_set_hexpand(picture, TRUE);

    GtkWidget* spinner = adw_spinner_new();
    gtk_widget_set_size_request(spinner, 48, 48);
    gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
    GtkWidget* failed = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(failed), "image-missing-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(failed), "Não foi possível carregar a imagem");

    GtkWidget* stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(stack), spinner, "loading");
    gtk_stack_add_named(GTK_STACK(stack), picture, "picture");
    gtk_stack_add_named(GTK_STACK(stack), failed, "failed");
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "loading");
    gtk_widget_set_margin_start(stack, 12);
    gtk_widget_set_margin_end(stack, 12);
    gtk_widget_set_margin_bottom(stack, 12);
    // Shown when the texture lands (immediately on a memory hit).
    onNotify(picture, "notify::paintable", [stack] { gtk_stack_set_visible_child_name(GTK_STACK(stack), "picture"); });

    GtkWidget* view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), stack);
    adw_dialog_set_child(dialog, view);
    adw_dialog_present(dialog, parent);

    // As big as a large screen shows it; the original can be far bigger.
    loadInto(GTK_PICTURE(picture), url, 2560, [stack] {
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "failed");
    });
}

}  // namespace cardinal::gtk
