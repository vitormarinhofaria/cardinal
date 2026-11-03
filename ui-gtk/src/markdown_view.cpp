#include "markdown_view.h"

#include <algorithm>
#include <memory>

#include "cardinal/markdown.h"

namespace cardinal::gtk {
namespace {

using md::Block;
using md::Inline;
using md::Span;

constexpr const char* kCardScheme = "cardinal-card:";
constexpr int kMarkerWidth = 22;  // px of a list bullet's column
constexpr int kQuoteIndent = 14;

bool isWeb(const std::string& url) { return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0; }

std::string escape(std::string_view s) {
    gchar* e = g_markup_escape_text(s.data(), static_cast<gssize>(s.size()));
    std::string out = e;
    g_free(e);
    return out;
}

void openUri(GtkWidget* from, const std::string& url) {
    GtkUriLauncher* launcher = gtk_uri_launcher_new(url.c_str());
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(gtk_widget_get_root(from)), nullptr, nullptr, nullptr);
    g_object_unref(launcher);
}

// Shared by every widget of one rendering: the signals of each keep it alive.
using OpenCard = std::function<void(const std::string& cardId)>;

class Renderer {
public:
    explicit Renderer(const MarkdownContext& ctx)
        : ctx_(ctx), openCard_(std::make_shared<OpenCard>(ctx.openCard)) {}

    GtkWidget* render(std::string_view text) {
        GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        addClass(root, "markdown");
        for (const Block& b : md::parse(text)) gtk_box_append(GTK_BOX(root), placed(b, widgetFor(b)));
        return root;
    }

private:
    const CardLink* cardFor(const std::string& shortLink) const {
        if (!ctx_.cards) return nullptr;
        for (const auto& c : *ctx_.cards)
            if (c.shortLink == shortLink) return &c;
        return nullptr;
    }

    // A card this span links to, when it is known (else the span is an ordinary link).
    const CardLink* resolvedCard(const Span& s) const {
        if (s.cardShortLink.empty()) return nullptr;
        const CardLink* c = cardFor(s.cardShortLink);
        return c && c->status == CardLink::Status::Resolved && !c->cardId.empty() ? c : nullptr;
    }

    std::string markup(const Inline& in) const {
        std::string out;
        for (const Span& s : in) {
            if (s.lineBreak) {
                out += '\n';
                continue;
            }
            std::string text = s.text;
            std::string href = s.link;
            if (const CardLink* card = resolvedCard(s)) {
                href = kCardScheme + card->cardId;
                if (s.cardName) text = card->name.empty() ? s.text : card->name;
            }
            std::string piece = escape(text);
            if (s.code) piece = "<span font_family=\"monospace\" bgcolor=\"#8080802e\">" + piece + "</span>";
            if (s.strike) piece = "<s>" + piece + "</s>";
            if (s.italic) piece = "<i>" + piece + "</i>";
            if (s.bold) piece = "<b>" + piece + "</b>";
            if (!href.empty()) {
                const std::string tip = href.rfind(kCardScheme, 0) == 0 ? "Abrir o card" : href;
                piece = "<a href=\"" + escape(href) + "\" title=\"" + escape(tip) + "\">" + piece + "</a>";
            }
            out += piece;
        }
        return out;
    }

    void connectLinks(GtkWidget* label) const {
        g_signal_connect_data(
            label, "activate-link",
            G_CALLBACK(+[](GtkLabel* l, const char* uri, gpointer data) -> gboolean {
                const std::string u = uri ? uri : "";
                auto& open = *static_cast<std::shared_ptr<OpenCard>*>(data);
                if (u.rfind(kCardScheme, 0) == 0) {
                    if (*open) (*open)(u.substr(std::string_view(kCardScheme).size()));
                } else if (isWeb(u)) {
                    openUri(GTK_WIDGET(l), u);
                }
                return TRUE;  // never let GTK open other schemes (file:, custom handlers)
            }),
            new std::shared_ptr<OpenCard>(openCard_),
            [](gpointer d, GClosure*) { delete static_cast<std::shared_ptr<OpenCard>*>(d); }, G_CONNECT_DEFAULT);
    }

    GtkWidget* textLabel(const Inline& in, const char* cls = nullptr) const {
        GtkWidget* l = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(l), markup(in).c_str());
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
        // wrap to whatever width it gets instead of asking for the text's
        gtk_label_set_width_chars(GTK_LABEL(l), 1);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 1);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_label_set_selectable(GTK_LABEL(l), ctx_.selectable);
        if (cls) addClass(l, cls);
        connectLinks(l);
        return l;
    }

    // A paragraph that is just a link to a card shows the card as a chip.
    GtkWidget* cardChip(const Inline& in) const {
        if (in.size() != 1 || !in[0].cardName) return nullptr;
        const CardLink* card = resolvedCard(in[0]);
        if (!card) return nullptr;
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name("view-paged-symbolic"));
        GtkWidget* name = label(card->name.empty() ? in[0].text : card->name);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_box_append(GTK_BOX(box), name);
        GtkWidget* button = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(button), box);
        addClass(button, "card-link-chip");
        gtk_widget_set_halign(button, GTK_ALIGN_FILL);
        gtk_widget_set_tooltip_text(button, (card->name + "\n" + in[0].link).c_str());
        on(button, "clicked", [open = openCard_, id = card->cardId] {
            if (*open) (*open)(id);
        });
        return button;
    }

    GtkWidget* picture(const Block& b) const {
        const std::string title = b.text.empty() ? b.url.substr(b.url.find_last_of('/') + 1) : b.text;
        if (!ctx_.images || !isWeb(b.url)) {
            Span link;
            link.text = title;
            link.link = b.url;
            return textLabel(Inline{link});
        }
        const std::optional<std::string> fetch = ctx_.pictureUrl ? ctx_.pictureUrl(b.url) : b.url;
        const int width = ctx_.pictureWidth;
        GtkWidget* pic = gtk_picture_new();
        gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
        gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_SCALE_DOWN);
        gtk_widget_set_size_request(pic, std::min(width, 220), 120);  // until it loads
        addClass(pic, "thumb");
        addClass(pic, "thumb-loading");
        gtk_widget_set_overflow(pic, GTK_OVERFLOW_HIDDEN);

        GtkWidget* button = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(button), pic);
        addClass(button, "flat");
        addClass(button, "thumb-button");
        gtk_widget_set_halign(button, GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(button, title.c_str());
        on(button, "clicked", [images = ctx_.images, button, url = b.url, title] {
            images->showViewer(button, url, title);
        });
        if (!fetch) return button;  // which version to fetch is not known yet
        onNotify(pic, "notify::paintable", [pic] {
            // A shrinkable picture asks for no height at all, and a box short of room squeezes
            // it to nothing: ask for the decoded height (already at most pictureWidth wide).
            GdkPaintable* p = gtk_picture_get_paintable(GTK_PICTURE(pic));
            gtk_widget_set_size_request(pic, -1, p ? gdk_paintable_get_intrinsic_height(p) : -1);
            gtk_widget_remove_css_class(pic, "thumb-loading");
        });
        ctx_.images->loadInto(GTK_PICTURE(pic), *fetch, width, [button, title] {
            GtkWidget* failed = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_box_append(GTK_BOX(failed), gtk_image_new_from_icon_name("image-missing-symbolic"));
            gtk_box_append(GTK_BOX(failed), label(title, "dim-label"));
            gtk_button_set_child(GTK_BUTTON(button), failed);
            gtk_widget_set_tooltip_text(button, "Não foi possível carregar a imagem");
        });
        return button;
    }

    GtkWidget* code(const Block& b) const {
        GtkWidget* l = gtk_label_new(b.text.c_str());
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_selectable(GTK_LABEL(l), TRUE);
        addClass(l, "md-code-text");
        GtkWidget* scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), l);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
        addClass(scroll, "md-code");
        return scroll;
    }

    GtkWidget* table(const Block& b) const {
        GtkWidget* grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        addClass(grid, "md-table");
        for (size_t r = 0; r < b.rows.size(); ++r)
            for (size_t c = 0; c < b.rows[r].size(); ++c) {
                GtkWidget* cell = textLabel(b.rows[r][c], r == 0 ? "md-th" : nullptr);
                // cells keep their line: a wide table scrolls sideways instead
                gtk_label_set_wrap(GTK_LABEL(cell), FALSE);
                gtk_label_set_width_chars(GTK_LABEL(cell), -1);
                gtk_label_set_max_width_chars(GTK_LABEL(cell), -1);
                gtk_widget_set_hexpand(cell, FALSE);
                gtk_grid_attach(GTK_GRID(grid), cell, static_cast<int>(c), static_cast<int>(r), 1, 1);
            }
        GtkWidget* scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
        return scroll;
    }

    GtkWidget* widgetFor(const Block& b) const {
        switch (b.kind) {
            case Block::Kind::Heading: {
                static constexpr const char* kLevel[] = {"md-h1", "md-h2", "md-h3"};
                return textLabel(b.spans, kLevel[std::clamp(b.level, 1, 3) - 1]);
            }
            case Block::Kind::Code: return code(b);
            case Block::Kind::Rule: return gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
            case Block::Kind::Image: return picture(b);
            case Block::Kind::Table: return table(b);
            case Block::Kind::Paragraph:
                if (GtkWidget* chip = cardChip(b.spans)) return chip;
                return textLabel(b.spans);
        }
        return textLabel(b.spans);
    }

    // Indented for its quotes and list items, with its bullet when it starts an item.
    GtkWidget* placed(const Block& b, GtkWidget* w) const {
        GtkWidget* out = w;
        if (!b.marker.empty()) {
            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
            GtkWidget* bullet = label(b.marker, "md-marker", 1);
            gtk_widget_set_size_request(bullet, kMarkerWidth, -1);
            gtk_widget_set_valign(bullet, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(row), bullet);
            gtk_box_append(GTK_BOX(row), w);
            out = row;
        }
        int indent = std::max(0, b.listDepth - 1) * (kMarkerWidth + 4);
        if (b.marker.empty() && b.listDepth > 0) indent += kMarkerWidth + 4;  // continues an item
        if (b.quoteDepth > 0) {
            indent += (b.quoteDepth - 1) * kQuoteIndent;
            GtkWidget* quote = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            addClass(quote, "md-quote");
            gtk_box_append(GTK_BOX(quote), out);
            out = quote;
        }
        gtk_widget_set_margin_start(out, indent);
        return out;
    }

    const MarkdownContext& ctx_;
    std::shared_ptr<OpenCard> openCard_;
};

}  // namespace

GtkWidget* renderMarkdown(std::string_view text, const MarkdownContext& ctx) { return Renderer(ctx).render(text); }

}  // namespace cardinal::gtk
