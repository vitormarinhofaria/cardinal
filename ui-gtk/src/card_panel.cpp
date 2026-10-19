#include "card_panel.h"

#include <algorithm>

namespace cardinal::gtk {
namespace {

std::string formatDate(const std::string& iso) {
    GDateTime* utc = g_date_time_new_from_iso8601(iso.c_str(), nullptr);
    if (!utc) return iso;
    GDateTime* local = g_date_time_to_local(utc);
    gchar* text = g_date_time_format(local, "%d/%m/%Y %H:%M");
    std::string out = text ? text : iso;
    g_free(text);
    g_date_time_unref(local);
    g_date_time_unref(utc);
    return out;
}

GtkWidget* chip(const LabelChip& l) {
    GtkWidget* w = gtk_label_new(l.name.empty() ? " " : l.name.c_str());
    addClass(w, "label-chip");
    addClass(w, labelClass(l.color).c_str());
    return w;
}

GtkWidget* avatar(const std::string& name, int size) {
    GtkWidget* a = adw_avatar_new(size, name.c_str(), TRUE);
    gtk_widget_set_tooltip_text(a, name.c_str());
    return a;
}

std::string textOf(GtkTextBuffer* b) {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    gchar* t = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

std::string trimmed(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

GtkWidget* section(const char* title) { return label(title, "section-title"); }

GtkWidget* textArea(GtkTextBuffer** bufferOut, int minHeight) {
    GtkWidget* view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 6);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 6);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 8);
    addClass(view, "description-view");
    *bufferOut = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), minHeight);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    g_object_set_data(G_OBJECT(scroll), "view", view);
    return scroll;
}

// A "+ Etiqueta" style button with a popover holding a list.
GtkWidget* pickerButton(const char* text, GtkWidget** listOut) {
    GtkWidget* button = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(button), text);
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "list-add-symbolic");
    addClass(button, "flat");
    GtkWidget* popover = gtk_popover_new();
    *listOut = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(*listOut), GTK_SELECTION_NONE);
    addClass(*listOut, "navigation-sidebar");
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), *listOut);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
    gtk_widget_set_size_request(scroll, 240, -1);
    gtk_popover_set_child(GTK_POPOVER(popover), scroll);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
    return button;
}

void clearListBox(GtkWidget* list) {
    while (GtkWidget* c = gtk_widget_get_first_child(list)) gtk_list_box_remove(GTK_LIST_BOX(list), c);
}

}  // namespace

CardPanel::CardPanel(std::shared_ptr<CardPanelVM> vm, std::function<void()> onClose)
    : vm_(std::move(vm)), onClose_(std::move(onClose)) {
    // ---- header: title + close
    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(header, 12);
    gtk_widget_set_margin_start(header, 16);
    gtk_widget_set_margin_end(header, 8);
    title_ = gtk_editable_label_new("");
    gtk_widget_set_hexpand(title_, TRUE);
    addClass(title_, "panel-title");
    onNotify(title_, "notify::editing", [this] {
        if (!gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(title_))) commitTitle();
    });
    GtkWidget* close = gtk_button_new_from_icon_name("window-close-symbolic");
    addClass(close, "flat");
    addClass(close, "circular");
    gtk_widget_set_valign(close, GTK_ALIGN_START);
    on(close, "clicked", [this] { onClose_(); });
    gtk_box_append(GTK_BOX(header), title_);
    gtk_box_append(GTK_BOX(header), close);

    banner_ = adw_banner_new("");
    adw_banner_set_button_label(ADW_BANNER(banner_), "Dispensar");
    on(banner_, "button-clicked", [this] { vm_->dismissNotices(); });

    // ---- content
    content_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(content_, 16);
    gtk_widget_set_margin_end(content_, 16);
    gtk_widget_set_margin_bottom(content_, 16);

    listName_ = label("", "dim-label");
    gtk_box_append(GTK_BOX(content_), listName_);

    gtk_box_append(GTK_BOX(content_), section("Etiquetas"));
    GtkWidget* labelsRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    labelsBox_ = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(labelsBox_), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(labelsBox_), 6);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(labelsBox_), 6);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(labelsBox_), 6);
    gtk_widget_set_hexpand(labelsBox_, TRUE);
    addLabelButton_ = pickerButton("Etiqueta", &addLabelList_);
    g_signal_connect_data(
        addLabelList_, "row-activated",
        G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
            auto* self = static_cast<CardPanel*>(data);
            size_t i = static_cast<size_t>(gtk_list_box_row_get_index(row));
            gtk_menu_button_popdown(GTK_MENU_BUTTON(self->addLabelButton_));
            if (i < self->labelRowIds_.size()) self->vm_->addLabel(self->labelRowIds_[i]);
        }),
        this, nullptr, G_CONNECT_DEFAULT);
    gtk_box_append(GTK_BOX(labelsRow), labelsBox_);
    gtk_box_append(GTK_BOX(labelsRow), addLabelButton_);
    gtk_box_append(GTK_BOX(content_), labelsRow);

    gtk_box_append(GTK_BOX(content_), section("Membros"));
    GtkWidget* membersRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    membersBox_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_hexpand(membersBox_, TRUE);
    addMemberButton_ = pickerButton("Membro", &addMemberList_);
    g_signal_connect_data(
        addMemberList_, "row-activated",
        G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
            auto* self = static_cast<CardPanel*>(data);
            size_t i = static_cast<size_t>(gtk_list_box_row_get_index(row));
            gtk_menu_button_popdown(GTK_MENU_BUTTON(self->addMemberButton_));
            if (i < self->memberRowIds_.size()) self->vm_->addMember(self->memberRowIds_[i]);
        }),
        this, nullptr, G_CONNECT_DEFAULT);
    gtk_box_append(GTK_BOX(membersRow), membersBox_);
    gtk_box_append(GTK_BOX(membersRow), addMemberButton_);
    gtk_box_append(GTK_BOX(content_), membersRow);

    gtk_box_append(GTK_BOX(content_), section("Descrição"));
    GtkWidget* descScroll = textArea(&descBuffer_, 96);
    descView_ = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(descScroll), "view"));
    on(descBuffer_, "changed", [this] { descriptionEdited(); });
    gtk_box_append(GTK_BOX(content_), descScroll);
    descButtons_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(descButtons_, GTK_ALIGN_END);
    GtkWidget* save = gtk_button_new_with_label("Salvar");
    addClass(save, "suggested-action");
    on(save, "clicked", [this] { saveDescription(); });
    GtkWidget* revert = gtk_button_new_with_label("Cancelar");
    on(revert, "clicked", [this] { revertDescription(); });
    gtk_box_append(GTK_BOX(descButtons_), revert);
    gtk_box_append(GTK_BOX(descButtons_), save);
    gtk_widget_set_visible(descButtons_, FALSE);
    gtk_box_append(GTK_BOX(content_), descButtons_);

    extrasSpinner_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* sp = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(sp));
    gtk_box_append(GTK_BOX(extrasSpinner_), sp);
    gtk_box_append(GTK_BOX(extrasSpinner_), label("Carregando checklists e anexos…", "dim-label"));
    gtk_box_append(GTK_BOX(content_), extrasSpinner_);
    checklistsBox_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(content_), checklistsBox_);
    attachmentsBox_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(content_), attachmentsBox_);

    gtk_box_append(GTK_BOX(content_), section("Comentários"));
    GtkWidget* commentScroll = textArea(&commentBuffer_, 56);
    commentView_ = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(commentScroll), "view"));
    gtk_box_append(GTK_BOX(content_), commentScroll);
    sendButton_ = gtk_button_new_with_label("Comentar");
    addClass(sendButton_, "suggested-action");
    gtk_widget_set_halign(sendButton_, GTK_ALIGN_END);
    gtk_widget_set_sensitive(sendButton_, FALSE);
    on(sendButton_, "clicked", [this] { sendComment(); });
    on(commentBuffer_, "changed", [this] {
        gtk_widget_set_sensitive(sendButton_, !trimmed(textOf(commentBuffer_)).empty());
    });
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect_data(
        keys, "key-pressed",
        G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType mods, gpointer data) -> gboolean {
            if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && (mods & GDK_CONTROL_MASK)) {
                static_cast<CardPanel*>(data)->sendComment();
                return TRUE;
            }
            return FALSE;
        }),
        this, nullptr, G_CONNECT_DEFAULT);
    gtk_widget_add_controller(commentView_, keys);
    gtk_box_append(GTK_BOX(content_), sendButton_);
    commentsBox_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(content_), commentsBox_);

    GtkWidget* contentScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(contentScroll), content_);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(contentScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(contentScroll, TRUE);

    GtkWidget* loading = adw_status_page_new();
    GtkWidget* spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_size_request(spinner, 32, 32);
    adw_status_page_set_child(ADW_STATUS_PAGE(loading), spinner);

    stack_ = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(stack_), loading, "loading");
    gtk_stack_add_named(GTK_STACK(stack_), contentScroll, "content");
    gtk_widget_set_vexpand(stack_, TRUE);

    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    addClass(root_, "panel");
    gtk_box_append(GTK_BOX(root_), header);
    gtk_box_append(GTK_BOX(root_), banner_);
    gtk_box_append(GTK_BOX(root_), stack_);

    conn_ = vm_->state().changed.connect([this](const CardPanelState& s) { update(s); });
    update(vm_->state().get());
}

CardPanel::~CardPanel() { vm_->state().changed.disconnect(conn_); }

std::string CardPanel::descriptionText() const { return textOf(descBuffer_); }

void CardPanel::setDescriptionText(const std::string& text) {
    settingText_ = true;
    gtk_text_buffer_set_text(descBuffer_, text.c_str(), -1);
    settingText_ = false;
}

void CardPanel::commitTitle() {
    const std::string text = trimmed(str(gtk_editable_get_text(GTK_EDITABLE(title_))));
    if (text.empty() || text == shownTitle_) {
        gtk_editable_set_text(GTK_EDITABLE(title_), shownTitle_.c_str());
        return;
    }
    vm_->setTitle(text);
}

void CardPanel::descriptionEdited() {
    if (settingText_) return;
    gtk_widget_set_visible(descButtons_, descriptionText() != shownDescription_);
}

void CardPanel::saveDescription() {
    vm_->setDescription(descriptionText());
    gtk_widget_set_visible(descButtons_, FALSE);
}

void CardPanel::revertDescription() {
    setDescriptionText(shownDescription_);
    gtk_widget_set_visible(descButtons_, FALSE);
}

void CardPanel::selfTest() {
    gtk_editable_set_text(GTK_EDITABLE(title_), "Título editado pelo autoteste");
    commitTitle();
    gtk_text_buffer_set_text(descBuffer_, "Descrição editada pelo autoteste ☕", -1);
    saveDescription();
    if (GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(addLabelList_), 0))
        g_signal_emit_by_name(addLabelList_, "row-activated", row);
    if (GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(addMemberList_), 0))
        g_signal_emit_by_name(addMemberList_, "row-activated", row);
    gtk_text_buffer_set_text(commentBuffer_, "Comentário enviado pelo autoteste 🚀", -1);
    sendComment();
}

void CardPanel::sendComment() {
    const std::string text = trimmed(textOf(commentBuffer_));
    if (text.empty()) return;
    vm_->addComment(text);
    gtk_text_buffer_set_text(commentBuffer_, "", -1);
}

void CardPanel::update(const CardPanelState& s) {
    using Status = CardPanelState::Status;
    if (s.status == Status::Loading) {
        gtk_stack_set_visible_child_name(GTK_STACK(stack_), "loading");
        shownStatus_ = s.status;
        return;
    }
    if (!tracedReady_) {
        tracedReady_ = true;
        trace("panel: state ready");
        traceNextFrame(root_, "panel: first frame with the card");
    }
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "content");
    const bool removed = s.status == Status::Removed;
    gtk_widget_set_sensitive(content_, !removed);
    shownStatus_ = s.status;

    updateNotice(s);

    if (s.title != shownTitle_) {
        shownTitle_ = s.title;
        if (!gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(title_)))
            gtk_editable_set_text(GTK_EDITABLE(title_), s.title.c_str());
    }
    gtk_label_set_text(GTK_LABEL(listName_), s.listName.empty() ? "" : ("na lista " + s.listName).c_str());
    gtk_widget_set_visible(listName_, !s.listName.empty());

    // never clobber what the user is typing; follow remote changes only when nothing is pending
    if (s.description != shownDescription_) {
        const bool editing = descriptionText() != shownDescription_;
        shownDescription_ = s.description;
        if (!editing) {
            setDescriptionText(s.description);
            gtk_widget_set_visible(descButtons_, FALSE);
        } else {
            gtk_widget_set_visible(descButtons_, descriptionText() != shownDescription_);
        }
    }

    updateLabels(s);
    updateMembers(s);
    updateComments(s);
    updateExtras(s);
}

void CardPanel::updateNotice(const CardPanelState& s) {
    if (s.notices == shownNotices_) return;
    shownNotices_ = s.notices;
    if (s.notices.empty()) {
        adw_banner_set_revealed(ADW_BANNER(banner_), FALSE);
        return;
    }
    const PanelNotice& n = s.notices.front();
    std::string text;
    switch (n.kind) {
        case PanelNotice::Kind::RemovedRemotely:
            text = "Este card foi arquivado ou excluído por outra pessoa.";
            break;
        case PanelNotice::Kind::ConflictOverwritten:
            text = std::string("Você sobrescreveu uma alteração de outra pessoa ") +
                   (n.field == "name" ? "no título." : "na descrição.");
            break;
        case PanelNotice::Kind::WriteRejected:
            text = "O Trello recusou uma alteração; ela foi desfeita.";
            break;
    }
    adw_banner_set_title(ADW_BANNER(banner_), text.c_str());
    adw_banner_set_revealed(ADW_BANNER(banner_), TRUE);
}

void CardPanel::updateLabels(const CardPanelState& s) {
    if (s.labels != shownLabels_) {
        shownLabels_ = s.labels;
        while (GtkWidget* c = gtk_widget_get_first_child(labelsBox_)) gtk_flow_box_remove(GTK_FLOW_BOX(labelsBox_), c);
        for (const auto& l : s.labels) gtk_flow_box_append(GTK_FLOW_BOX(labelsBox_), chip(l));
    }
    if (s.boardLabels != shownBoardLabels_ || s.labels != shownLabels_ || labelRowIds_.empty()) {
        shownBoardLabels_ = s.boardLabels;
        clearListBox(addLabelList_);
        labelRowIds_.clear();
        for (const auto& l : s.boardLabels) {
            if (std::any_of(s.labels.begin(), s.labels.end(), [&](const LabelChip& x) { return x.id == l.id; })) continue;
            labelRowIds_.push_back(l.id);
            GtkWidget* c = chip(l);
            gtk_widget_set_halign(c, GTK_ALIGN_START);
            gtk_widget_set_margin_top(c, 4);
            gtk_widget_set_margin_bottom(c, 4);
            gtk_widget_set_margin_start(c, 6);
            gtk_list_box_append(GTK_LIST_BOX(addLabelList_), c);
        }
        gtk_widget_set_sensitive(addLabelButton_, !labelRowIds_.empty());
    }
}

void CardPanel::updateMembers(const CardPanelState& s) {
    if (s.members != shownMembers_) {
        shownMembers_ = s.members;
        clearChildren(membersBox_);
        for (const auto& m : s.members) gtk_box_append(GTK_BOX(membersBox_), avatar(m.fullName, 30));
    }
    if (s.boardMembers != shownBoardMembers_ || memberRowIds_.empty()) {
        shownBoardMembers_ = s.boardMembers;
        clearListBox(addMemberList_);
        memberRowIds_.clear();
        for (const auto& m : s.boardMembers) {
            if (std::any_of(s.members.begin(), s.members.end(), [&](const MemberChip& x) { return x.id == m.id; })) continue;
            memberRowIds_.push_back(m.id);
            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_margin_top(row, 4);
            gtk_widget_set_margin_bottom(row, 4);
            gtk_widget_set_margin_start(row, 6);
            gtk_widget_set_margin_end(row, 6);
            gtk_box_append(GTK_BOX(row), avatar(m.fullName, 24));
            gtk_box_append(GTK_BOX(row), label(m.fullName));
            gtk_list_box_append(GTK_LIST_BOX(addMemberList_), row);
        }
        gtk_widget_set_sensitive(addMemberButton_, !memberRowIds_.empty());
    }
}

void CardPanel::updateComments(const CardPanelState& s) {
    if (s.comments == shownComments_) return;
    shownComments_ = s.comments;
    clearChildren(commentsBox_);
    for (const auto& c : s.comments) {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(row, GTK_ALIGN_START);
        GtkWidget* who = avatar(c.authorName.empty() ? "?" : c.authorName, 30);
        gtk_widget_set_valign(who, GTK_ALIGN_START);
        GtkWidget* bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        addClass(bubble, "comment-bubble");
        gtk_widget_set_hexpand(bubble, TRUE);
        GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* name = label(c.authorName.empty() ? "Alguém" : c.authorName, "heading");
        gtk_box_append(GTK_BOX(head), name);
        gtk_box_append(GTK_BOX(head), label(c.sending ? "enviando…" : formatDate(c.date), "dim-label"));
        GtkWidget* text = label(c.text);
        gtk_label_set_wrap(GTK_LABEL(text), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(text), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_selectable(GTK_LABEL(text), TRUE);
        gtk_label_set_width_chars(GTK_LABEL(text), 1);
        gtk_label_set_max_width_chars(GTK_LABEL(text), 1);
        gtk_widget_set_hexpand(text, TRUE);
        gtk_box_append(GTK_BOX(bubble), head);
        gtk_box_append(GTK_BOX(bubble), text);
        if (c.sending) addClass(row, "comment-sending");
        gtk_box_append(GTK_BOX(row), who);
        gtk_box_append(GTK_BOX(row), bubble);
        gtk_box_append(GTK_BOX(commentsBox_), row);
    }
}

void CardPanel::updateExtras(const CardPanelState& s) {
    if (s.extrasLoaded != shownExtrasLoaded_) {
        shownExtrasLoaded_ = s.extrasLoaded;
        gtk_widget_set_visible(extrasSpinner_, !s.extrasLoaded);
    }
    if (s.checklists != shownChecklists_) {
        shownChecklists_ = s.checklists;
        clearChildren(checklistsBox_);
        for (const auto& list : s.checklists) {
            gtk_box_append(GTK_BOX(checklistsBox_), section(list.name.c_str()));
            for (const auto& item : list.items) {
                GtkWidget* check = gtk_check_button_new_with_label(item.name.c_str());
                gtk_check_button_set_active(GTK_CHECK_BUTTON(check), item.done);
                gtk_widget_set_sensitive(check, FALSE);  // read-only in v1
                gtk_box_append(GTK_BOX(checklistsBox_), check);
            }
        }
    }
    if (s.attachments != shownAttachments_) {
        shownAttachments_ = s.attachments;
        clearChildren(attachmentsBox_);
        if (!s.attachments.empty()) gtk_box_append(GTK_BOX(attachmentsBox_), section("Anexos"));
        for (const auto& a : s.attachments) {
            GtkWidget* link = gtk_link_button_new_with_label(a.url.c_str(), a.name.empty() ? a.url.c_str() : a.name.c_str());
            gtk_widget_set_halign(link, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(attachmentsBox_), link);
        }
    }
}

}  // namespace cardinal::gtk
