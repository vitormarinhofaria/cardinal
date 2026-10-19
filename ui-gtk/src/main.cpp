#include <adwaita.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

#include "cardinal/credentials.h"
#include "cardinal/session.h"
#include "gtk_util.h"
#include "platform.h"
#include "style.h"
#include "window.h"

namespace {

using namespace cardinal;

// Development switches, so the app can run against tools/mock_trello.py with no real account.
struct Options {
    std::string apiBase;
    std::string apiKey;
    std::string token;
    std::string dataDir;
    int pollSeconds = 20;
    std::string snapshot;  // render the window to this PNG after snapshotDelayMs, then quit
    int snapshotDelayMs = 2500;
    bool noKeyring = false;  // keep credentials in memory only (development)
    std::string openCard;  // open this card as soon as the board shows (development)
};

class MemoryCredentials : public CredentialStore {
public:
    explicit MemoryCredentials(std::string token) {
        if (!token.empty()) value_ = Credentials{std::move(token)};
    }
    std::optional<Credentials> load() override { return value_; }
    void save(const Credentials& c) override { value_ = c; }
    void clear() override { value_.reset(); }

private:
    std::optional<Credentials> value_;
};

struct App {
    Options options;
    std::string dataDir;
    cardinal::gtk::GtkDispatcher dispatcher;
    cardinal::gtk::GtkBrowserOpener browser;
    std::unique_ptr<CredentialStore> credentials;
    std::unique_ptr<Session> session;
    std::unique_ptr<cardinal::gtk::MainWindow> window;
};

// Development aid: render the window to a PNG so the UI can be checked without a screenshot tool.
void saveWindowPng(GtkWindow* window, const std::string& path) {
    GtkWidget* w = GTK_WIDGET(window);
    const int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
    GdkPaintable* paintable = gtk_widget_paintable_new(w);
    GtkSnapshot* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, width, height);
    GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
    GskRenderer* renderer = gtk_native_get_renderer(GTK_NATIVE(window));
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, static_cast<float>(width), static_cast<float>(height));
    if (node && renderer) {
        GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &bounds);
        if (texture) {
            gdk_texture_save_to_png(texture, path.c_str());
            g_object_unref(texture);
        }
    }
    if (node) gsk_render_node_unref(node);
    g_object_unref(paintable);
}

void activate(GApplication* gapp, gpointer data) {
    auto* app = static_cast<App*>(data);
    cardinal::gtk::trace("activate");
    if (app->window) {
        gtk_window_present(app->window->window());
        return;
    }
    auto* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, cardinal::gtk::kStyleCss);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    const Options& o = app->options;
    AppConfig cfg;
    cfg.dataDir = app->dataDir;
    cfg.apiKey = !o.apiKey.empty() ? o.apiKey
                 : std::getenv("CARDINAL_API_KEY") ? std::getenv("CARDINAL_API_KEY")
                                                   : CARDINAL_TRELLO_API_KEY;
    if (!o.apiBase.empty()) cfg.apiBase = o.apiBase;
    cfg.pollInterval = std::chrono::seconds(o.pollSeconds);

    if (!o.token.empty() || o.noKeyring)
        app->credentials = std::make_unique<MemoryCredentials>(o.token);
    else
        app->credentials = makeLibsecretCredentialStore();

    app->session = std::make_unique<Session>(cfg, *app->credentials, app->browser, app->dispatcher);
    app->window = std::make_unique<cardinal::gtk::MainWindow>(ADW_APPLICATION(gapp), *app->session,
                                                              app->dataDir, o.openCard);
    gtk_window_present(app->window->window());
    cardinal::gtk::trace("window presented");
    cardinal::gtk::traceNextFrame(GTK_WIDGET(app->window->window()), "window: first frame (whatever it shows)");

    if (!o.snapshot.empty()) {
        g_timeout_add(static_cast<guint>(o.snapshotDelayMs), [](gpointer d) -> gboolean {
            auto* a = static_cast<App*>(d);
            saveWindowPng(a->window->window(), a->options.snapshot);
            g_application_quit(g_application_get_default());
            return G_SOURCE_REMOVE;
        }, app);
    }
}

}  // namespace

int main(int argc, char** argv) {
    cardinal::gtk::processStartUs();
    App app;
    adw_init();
    AdwApplication* gapp = adw_application_new("dev.cardinal.Cardinal", G_APPLICATION_DEFAULT_FLAGS);

    static gchar* apiBase = nullptr;
    static gchar* apiKey = nullptr;
    static gchar* token = nullptr;
    static gchar* dataDir = nullptr;
    static gint poll = 20;
    static gchar* snapshot = nullptr;
    static gint snapshotDelay = 2500;
    static gchar* openCard = nullptr;
    static gboolean noKeyring = FALSE;
    GOptionEntry entries[] = {
        {"api-base", 0, 0, G_OPTION_ARG_STRING, &apiBase, "Trello API base URL (development)", "URL"},
        {"api-key", 0, 0, G_OPTION_ARG_STRING, &apiKey, "Trello API key", "KEY"},
        {"token", 0, 0, G_OPTION_ARG_STRING, &token, "Use this token, skip login and keyring (development)", "TOKEN"},
        {"data-dir", 0, 0, G_OPTION_ARG_FILENAME, &dataDir, "Where the cache lives", "DIR"},
        {"poll-seconds", 0, 0, G_OPTION_ARG_INT, &poll, "Seconds between live-update polls", "N"},
        {"snapshot", 0, 0, G_OPTION_ARG_FILENAME, &snapshot, "Render the window to this PNG and quit (development)", "FILE"},
        {"snapshot-delay", 0, 0, G_OPTION_ARG_INT, &snapshotDelay, "Milliseconds to wait before the snapshot", "MS"},
        {"no-keyring", 0, 0, G_OPTION_ARG_NONE, &noKeyring, "Do not read or write the system keyring (development)", nullptr},
        {"open-card", 0, 0, G_OPTION_ARG_STRING, &openCard, "Open this card id once the board shows (development)", "ID"},
        {nullptr, 0, 0, G_OPTION_ARG_NONE, nullptr, nullptr, nullptr},
    };
    g_application_add_main_option_entries(G_APPLICATION(gapp), entries);
    g_signal_connect(gapp, "handle-local-options",
                     G_CALLBACK(+[](GApplication*, GVariantDict*, gpointer data) -> gint {
                         auto* a = static_cast<App*>(data);
                         a->options.apiBase = apiBase ? apiBase : "";
                         a->options.apiKey = apiKey ? apiKey : "";
                         a->options.token = token ? token : "";
                         a->options.pollSeconds = poll;
                         a->options.snapshot = snapshot ? snapshot : "";
                         a->options.snapshotDelayMs = snapshotDelay;
                         a->options.openCard = openCard ? openCard : "";
                         a->options.noKeyring = noKeyring;
                         a->dataDir = dataDir ? dataDir
                                              : (std::filesystem::path(g_get_user_data_dir()) / "cardinal").string();
                         return -1;
                     }),
                     &app);
    g_signal_connect(gapp, "activate", G_CALLBACK(activate), &app);
    int status = g_application_run(G_APPLICATION(gapp), argc, argv);
    app.window.reset();
    app.session.reset();
    g_object_unref(gapp);
    return status;
}
