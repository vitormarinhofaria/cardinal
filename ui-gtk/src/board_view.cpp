#include "board_view.h"

#include <algorithm>

namespace cardinal::gtk {
namespace {

GObject* makeItem(const CardRow& row) {
    GObject* o = static_cast<GObject*>(g_object_new(G_TYPE_OBJECT, nullptr));
    g_object_set_data_full(o, "row", new CardRow(row),
                           [](gpointer p) { delete static_cast<CardRow*>(p); });
    return o;
}

const CardRow* rowOf(gpointer item) {
    return static_cast<const CardRow*>(g_object_get_data(G_OBJECT(item), "row"));
}

// Keep unchanged rows (and their widgets) and replace only the stretch that differs.
void spliceDiff(GListStore* store, std::vector<CardRow>& shown, const std::vector<CardRow>& next) {
    size_t prefix = 0;
    while (prefix < shown.size() && prefix < next.size() && shown[prefix] == next[prefix]) ++prefix;
    size_t suffix = 0;
    while (suffix < shown.size() - prefix && suffix < next.size() - prefix &&
           shown[shown.size() - 1 - suffix] == next[next.size() - 1 - suffix])
        ++suffix;
    const size_t removeCount = shown.size() - prefix - suffix;
    const size_t addCount = next.size() - prefix - suffix;
    if (removeCount == 0 && addCount == 0) return;

    std::vector<gpointer> items;
    items.reserve(addCount);
    for (size_t i = 0; i < addCount; ++i) items.push_back(makeItem(next[prefix + i]));
    g_list_store_splice(store, static_cast<guint>(prefix), static_cast<guint>(removeCount),
                        items.data(), static_cast<guint>(addCount));
    for (gpointer o : items) g_object_unref(o);
    shown = next;
}

// What a card row's widgets are, kept on the row widget itself.
struct CardWidgets {
    GtkWidget* labels;
    GtkWidget* title;
    GtkWidget* footer;
    GtkWidget* commentIcon;
    GtkWidget* commentCount;
    GtkWidget* members;
    GtkListItem* item = nullptr;
    std::string cardId;
    BoardView::Column* column = nullptr;
};

}  // namespace

struct BoardView::Column {
    BoardView* owner = nullptr;
    std::string id;
    GtkWidget* root = nullptr;
    GtkWidget* title = nullptr;
    GtkWidget* count = nullptr;
    GtkWidget* stack = nullptr;
    GtkWidget* coldCount = nullptr;
    GtkWidget* listView = nullptr;
    GtkWidget* addStack = nullptr;
    GtkWidget* addEntry = nullptr;
    GListStore* store = nullptr;
    GtkSelectionModel* selection = nullptr;  // attached to the list view only when near the screen
    bool attached = false;
    GtkWidget* scroller = nullptr;
    size_t limit = 40;  // rows handed to the list view; grows as the user scrolls down
    std::vector<CardRow> shown;
};

namespace {

void setupCard(GtkListItemFactory*, GtkListItem* item, gpointer ctx) {
    auto* column = static_cast<BoardView::Column*>(ctx);
    auto* w = new CardWidgets();
    w->column = column;
    w->item = item;

    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    addClass(card, "card-row");
    w->labels = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(card), w->labels);

    w->title = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(w->title), 0);
    gtk_label_set_wrap(GTK_LABEL(w->title), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(w->title), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_width_chars(GTK_LABEL(w->title), 1);
    gtk_label_set_max_width_chars(GTK_LABEL(w->title), 1);
    gtk_widget_set_hexpand(w->title, TRUE);
    gtk_box_append(GTK_BOX(card), w->title);

    w->footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* comments = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    addClass(comments, "card-meta");
    w->commentIcon = gtk_image_new_from_icon_name("chat-bubble-text-symbolic");
    w->commentCount = gtk_label_new("");
    gtk_box_append(GTK_BOX(comments), w->commentIcon);
    gtk_box_append(GTK_BOX(comments), w->commentCount);
    gtk_box_append(GTK_BOX(w->footer), comments);
    w->members = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(w->members, TRUE);
    gtk_widget_set_halign(w->members, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(w->footer), w->members);
    gtk_box_append(GTK_BOX(card), w->footer);

    // drag: the payload is just the card id
    GtkDragSource* drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
    g_signal_connect_data(
        drag, "prepare",
        G_CALLBACK(+[](GtkDragSource*, double, double, gpointer data) -> GdkContentProvider* {
            auto* cw = static_cast<CardWidgets*>(data);
            return gdk_content_provider_new_typed(G_TYPE_STRING, cw->cardId.c_str());
        }),
        w, nullptr, G_CONNECT_DEFAULT);
    g_signal_connect_data(
        drag, "drag-begin",
        G_CALLBACK(+[](GtkDragSource* source, GdkDrag*, gpointer data) {
            auto* cw = static_cast<CardWidgets*>(data);
            GdkPaintable* p = gtk_widget_paintable_new(gtk_list_item_get_child(cw->item));
            gtk_drag_source_set_icon(source, p, 0, 0);
            g_object_unref(p);
        }),
        w, nullptr, G_CONNECT_DEFAULT);
    gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(drag));

    // drop on a card: insert before it (upper half) or after it (lower half)
    GtkDropTarget* drop = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
    g_signal_connect_data(
        drop, "drop",
        G_CALLBACK(+[](GtkDropTarget* target, const GValue* value, double, double y, gpointer data) -> gboolean {
            auto* cw = static_cast<CardWidgets*>(data);
            GtkWidget* card = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
            gtk_widget_remove_css_class(card, "drop-target");
            const char* id = g_value_get_string(value);
            if (!id || !cw->column) return FALSE;
            const int pos = static_cast<int>(gtk_list_item_get_position(cw->item));
            const bool before = y < gtk_widget_get_height(card) / 2.0;
            cw->column->owner->dropCard(id, cw->column->id, before ? pos : pos + 1);
            return TRUE;
        }),
        w, nullptr, G_CONNECT_DEFAULT);
    g_signal_connect_data(
        drop, "enter",
        G_CALLBACK(+[](GtkDropTarget* target, double, double, gpointer) -> GdkDragAction {
            gtk_widget_add_css_class(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)), "drop-target");
            return GDK_ACTION_MOVE;
        }),
        nullptr, nullptr, G_CONNECT_DEFAULT);
    g_signal_connect_data(
        drop, "leave",
        G_CALLBACK(+[](GtkDropTarget* target, gpointer) {
            gtk_widget_remove_css_class(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)), "drop-target");
        }),
        nullptr, nullptr, G_CONNECT_DEFAULT);
    gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(drop));

    g_object_set_data_full(G_OBJECT(card), "widgets", w,
                           [](gpointer p) { delete static_cast<CardWidgets*>(p); });
    gtk_list_item_set_child(item, card);
}

void bindCard(GtkListItemFactory*, GtkListItem* item, gpointer) {
    GtkWidget* card = gtk_list_item_get_child(item);
    auto* w = static_cast<CardWidgets*>(g_object_get_data(G_OBJECT(card), "widgets"));
    const CardRow* row = rowOf(gtk_list_item_get_item(item));
    if (!row) return;
    w->item = item;
    w->cardId = row->id;

    clearChildren(w->labels);
    for (const auto& l : row->labels) {
        GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        addClass(bar, "label-bar");
        addClass(bar, labelClass(l.color).c_str());
        gtk_widget_set_tooltip_text(bar, l.name.c_str());
        gtk_box_append(GTK_BOX(w->labels), bar);
    }
    gtk_widget_set_visible(w->labels, !row->labels.empty());
    gtk_label_set_text(GTK_LABEL(w->title), row->title.c_str());

    const bool hasComments = row->commentCount > 0;
    gtk_label_set_text(GTK_LABEL(w->commentCount), std::to_string(row->commentCount).c_str());
    gtk_widget_set_visible(gtk_widget_get_parent(w->commentIcon), hasComments);
    clearChildren(w->members);
    for (const auto& m : row->members) {
        GtkWidget* avatar = adw_avatar_new(22, m.fullName.c_str(), TRUE);
        gtk_widget_set_tooltip_text(avatar, m.fullName.c_str());
        gtk_box_append(GTK_BOX(w->members), avatar);
    }
    gtk_widget_set_visible(w->footer, hasComments || !row->members.empty());
}

}  // namespace

BoardView::BoardView(std::shared_ptr<BoardVM> vm, std::function<void(const std::string&)> onOpenCard)
    : vm_(std::move(vm)), onOpenCard_(std::move(onOpenCard)) {
    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(box_, 12);
    gtk_widget_set_margin_bottom(box_, 12);
    gtk_widget_set_margin_start(box_, 12);
    gtk_widget_set_margin_end(box_, 12);

    root_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(root_), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(root_), box_);
    addClass(root_, "board-area");

    GtkAdjustment* h = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(root_));
    on(h, "value-changed", [this] { markVisibilityDirty(); });
    on(h, "changed", [this] { markVisibilityDirty(); });  // page size or width changed
    structureConn_ = vm_->structureChanged.connect([this] { rebuildStructure(); });
    columnConn_ = vm_->columnChanged.connect([this](const std::string& id) { updateColumn(id); });
    rebuildStructure();
}

BoardView::~BoardView() {
    *alive_ = false;
    vm_->structureChanged.disconnect(structureConn_);
    vm_->columnChanged.disconnect(columnConn_);
}

BoardView::Column* BoardView::ensureColumn(const std::string& listId) {
    auto it = columns_.find(listId);
    if (it != columns_.end()) return it->second;

    auto* c = new Column();
    c->owner = this;
    c->id = listId;
    c->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    addClass(c->root, "column");
    gtk_widget_set_size_request(c->root, 288, -1);
    gtk_widget_set_hexpand(c->root, FALSE);
    g_object_set_data_full(G_OBJECT(c->root), "column", c, [](gpointer p) {
        auto* col = static_cast<Column*>(p);
        if (col->selection) g_object_unref(col->selection);
        if (col->store) g_object_unref(col->store);
        delete col;
    });

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(header, 6);
    gtk_widget_set_margin_end(header, 6);
    c->title = label("", "column-title");
    gtk_widget_set_hexpand(c->title, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(c->title), PANGO_ELLIPSIZE_END);
    c->count = label("", "column-count");
    gtk_box_append(GTK_BOX(header), c->title);
    gtk_box_append(GTK_BOX(header), c->count);
    gtk_box_append(GTK_BOX(c->root), header);

    c->store = g_list_store_new(G_TYPE_OBJECT);
    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect_data(factory, "setup", G_CALLBACK(setupCard), c, nullptr, G_CONNECT_DEFAULT);
    g_signal_connect_data(factory, "bind", G_CALLBACK(bindCard), c, nullptr, G_CONNECT_DEFAULT);
    // A GtkListView realizes every row of a model it is given before it knows how much room it
    // has, so off-screen columns get their model later (attachVisibleColumns).
    c->selection = GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(c->store)));
    c->listView = gtk_list_view_new(nullptr, factory);
    gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(c->listView), TRUE);
    gtk_widget_add_css_class(c->listView, "transparent");
    g_signal_connect_data(
        c->listView, "activate",
        G_CALLBACK(+[](GtkListView*, guint position, gpointer data) {
            auto* col = static_cast<Column*>(data);
            gpointer item = g_list_model_get_item(G_LIST_MODEL(col->store), position);
            if (!item) return;
            if (const CardRow* row = rowOf(item)) col->owner->openCard(row->id);
            g_object_unref(item);
        }),
        c, nullptr, G_CONNECT_DEFAULT);
    GtkWidget* scroller = gtk_scrolled_window_new();
    c->scroller = scroller;
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    // A GtkListView costs time per row it is given, not per row on screen: hand over a window of
    // the column and extend it when the user scrolls near its end.
    GtkAdjustment* vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
    on(vadj, "value-changed", [this, c, vadj] {
        const double left = gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_value(vadj) -
                            gtk_adjustment_get_page_size(vadj);
        auto col = vm_->column(c->id);
        if (left < 600 && col && c->shown.size() < col->cards.size()) {
            c->limit = c->shown.size() + 60;
            updateColumn(c->id);
        }
    });
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), c->listView);
    gtk_widget_set_vexpand(scroller, TRUE);

    GtkWidget* cold = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    addClass(cold, "column-cold");
    gtk_widget_set_valign(cold, GTK_ALIGN_START);
    c->coldCount = label("", "title-2", 0.5f);
    gtk_box_append(GTK_BOX(cold), c->coldCount);
    gtk_box_append(GTK_BOX(cold), label("cards ainda não carregados", "dim-label", 0.5f));
    GtkWidget* load = gtk_button_new_with_label("Carregar");
    gtk_widget_set_halign(load, GTK_ALIGN_CENTER);
    on(load, "clicked", [this, listId] { vm_->expandList(listId); });
    gtk_box_append(GTK_BOX(cold), load);

    c->stack = gtk_stack_new();
    gtk_widget_set_vexpand(c->stack, TRUE);
    gtk_stack_add_named(GTK_STACK(c->stack), scroller, "cards");
    gtk_stack_add_named(GTK_STACK(c->stack), cold, "cold");
    gtk_box_append(GTK_BOX(c->root), c->stack);

    // "+ Adicionar card": a button that turns into an entry
    c->addStack = gtk_stack_new();
    GtkWidget* addButton = gtk_button_new_with_label("+ Adicionar card");
    addClass(addButton, "flat");
    gtk_widget_set_halign(addButton, GTK_ALIGN_FILL);
    c->addEntry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(c->addEntry), "Título do card…");
    gtk_stack_add_named(GTK_STACK(c->addStack), addButton, "button");
    gtk_stack_add_named(GTK_STACK(c->addStack), c->addEntry, "entry");
    on(addButton, "clicked", [c] {
        gtk_stack_set_visible_child_name(GTK_STACK(c->addStack), "entry");
        gtk_widget_grab_focus(c->addEntry);
    });
    on(c->addEntry, "activate", [this, c, listId] {
        std::string text = str(gtk_editable_get_text(GTK_EDITABLE(c->addEntry)));
        if (text.empty()) return;
        vm_->createCard(listId, text);
        gtk_editable_set_text(GTK_EDITABLE(c->addEntry), "");  // stay open for the next one
    });
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect_data(
        keys, "key-pressed",
        G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) -> gboolean {
            if (keyval != GDK_KEY_Escape) return FALSE;
            auto* col = static_cast<Column*>(data);
            gtk_editable_set_text(GTK_EDITABLE(col->addEntry), "");
            gtk_stack_set_visible_child_name(GTK_STACK(col->addStack), "button");
            return TRUE;
        }),
        c, nullptr, G_CONNECT_DEFAULT);
    gtk_widget_add_controller(c->addEntry, keys);
    gtk_box_append(GTK_BOX(c->root), c->addStack);

    // drop on the column background or header: append to the end
    GtkDropTarget* drop = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
    g_signal_connect_data(
        drop, "drop",
        G_CALLBACK(+[](GtkDropTarget*, const GValue* value, double, double, gpointer data) -> gboolean {
            auto* col = static_cast<Column*>(data);
            gtk_widget_remove_css_class(col->root, "drop-target");
            const char* id = g_value_get_string(value);
            if (!id) return FALSE;
            col->owner->dropCard(id, col->id, static_cast<int>(col->shown.size()));
            return TRUE;
        }),
        c, nullptr, G_CONNECT_DEFAULT);
    g_signal_connect_data(
        drop, "enter",
        G_CALLBACK(+[](GtkDropTarget*, double, double, gpointer data) -> GdkDragAction {
            gtk_widget_add_css_class(static_cast<Column*>(data)->root, "drop-target");
            return GDK_ACTION_MOVE;
        }),
        c, nullptr, G_CONNECT_DEFAULT);
    g_signal_connect_data(
        drop, "leave",
        G_CALLBACK(+[](GtkDropTarget*, gpointer data) {
            gtk_widget_remove_css_class(static_cast<Column*>(data)->root, "drop-target");
        }),
        c, nullptr, G_CONNECT_DEFAULT);
    gtk_widget_add_controller(c->root, GTK_EVENT_CONTROLLER(drop));

    gtk_box_append(GTK_BOX(box_), c->root);
    columns_[listId] = c;
    return c;
}

void BoardView::markVisibilityDirty() {
    if (visibilityDirty_) return;
    visibilityDirty_ = true;
    struct Ctx {
        BoardView* self;
        std::shared_ptr<bool> alive;
    };
    gtk_widget_add_tick_callback(
        root_,
        [](GtkWidget*, GdkFrameClock*, gpointer data) -> gboolean {
            auto* ctx = static_cast<Ctx*>(data);
            if (*ctx->alive) {
                ctx->self->visibilityDirty_ = false;
                ctx->self->attachVisibleColumns();
            }
            return G_SOURCE_REMOVE;
        },
        new Ctx{this, alive_}, [](gpointer d) { delete static_cast<Ctx*>(d); });
}

// Give the list view its model once its column is within one column's width of the viewport.
void BoardView::attachVisibleColumns() {
    const float viewport = static_cast<float>(gtk_widget_get_width(root_));
    if (viewport <= 0) return;
    for (auto& [id, c] : columns_) {
        if (c->attached) continue;
        if (gtk_widget_get_width(c->root) <= 0) continue;  // not laid out yet: bounds mean nothing
        graphene_rect_t b;
        if (!gtk_widget_compute_bounds(c->root, root_, &b)) continue;
        if (b.origin.x + b.size.width < -300 || b.origin.x > viewport + 300) continue;
        c->attached = true;
        gtk_list_view_set_model(GTK_LIST_VIEW(c->listView), c->selection);
    }
}

void BoardView::updateColumn(const std::string& listId) {
    auto col = vm_->column(listId);
    if (!col) return;
    Column* c = ensureColumn(listId);
    gtk_label_set_text(GTK_LABEL(c->title), col->name.c_str());
    gtk_label_set_text(GTK_LABEL(c->count), std::to_string(col->cardCount).c_str());
    gtk_label_set_text(GTK_LABEL(c->coldCount), std::to_string(col->cardCount).c_str());
    gtk_stack_set_visible_child_name(GTK_STACK(c->stack), col->loaded ? "cards" : "cold");
    if (col->loaded) {
        const bool first = c->shown.empty() && !col->cards.empty();
        const size_t n = std::min(c->limit, col->cards.size());
        std::vector<CardRow> window(col->cards.begin(), col->cards.begin() + static_cast<long>(n));
        spliceDiff(c->store, c->shown, window);
        static bool traced = false;
        if (first && !traced) {
            traced = true;
            trace("board: first column with cards (" + col->name + ")");
            traceNextFrame(root_, "board: first frame with cards");
        }
    }
}

void BoardView::rebuildStructure() {
    trace("board: rebuildStructure begin (" + std::to_string(vm_->columns().size()) + " columns)");
    std::vector<std::string> order;
    for (const auto& col : vm_->columns()) order.push_back(col->id);

    // drop columns that no longer exist
    for (auto it = columns_.begin(); it != columns_.end();) {
        if (std::find(order.begin(), order.end(), it->first) == order.end()) {
            gtk_box_remove(GTK_BOX(box_), it->second->root);  // frees the Column with the widget
            it = columns_.erase(it);
        } else {
            ++it;
        }
    }
    GtkWidget* prev = nullptr;
    for (const auto& id : order) {
        updateColumn(id);
        Column* c = columns_[id];
        gtk_box_reorder_child_after(GTK_BOX(box_), c->root, prev);
        prev = c->root;
    }
    order_ = std::move(order);
    markVisibilityDirty();
    trace("board: rebuildStructure end");
}

void BoardView::dropCard(const std::string& cardId, const std::string& toListId, int index) {
    auto target = vm_->column(toListId);
    if (!target) return;
    // BoardVM counts positions without the moved card, so a card dragged down within its own
    // column leaves one slot behind it.
    for (size_t i = 0; i < target->cards.size(); ++i)
        if (target->cards[i].id == cardId && static_cast<int>(i) < index) {
            --index;
            break;
        }
    vm_->moveCard(cardId, toListId, static_cast<size_t>(std::max(0, index)));
}

}  // namespace cardinal::gtk
