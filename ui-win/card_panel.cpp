#include "pch.h"

#include "card_panel.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <limits>

#include "style.h"
#include "win_util.h"

namespace cardinal::win {
namespace {

using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using winrt::Windows::System::VirtualKey;

constexpr double kPaneWidth = 420;
constexpr double kInnerWidth = kPaneWidth - 32;  // content padding on both sides

std::string formatDate(const std::string& iso) {
    std::tm tm{};
    if (sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                    &tm.tm_min, &tm.tm_sec) != 6)
        return iso;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    const std::time_t t = _mkgmtime(&tm);
    std::tm local{};
    if (t == -1 || localtime_s(&local, &t) != 0) return iso;
    char buf[32];
    std::strftime(buf, sizeof buf, "%d/%m/%Y %H:%M", &local);
    return buf;
}

std::string trimmed(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// TextBox hands back "\r" for line breaks; the model (and Trello) use "\n".
std::string fromBox(winrt::hstring const& h) {
    std::string s = str(h);
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            out += '\n';
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

TextBlock section(const std::string& title) {
    auto t = text(title, 14);
    t.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
    t.Margin(ThicknessHelper::FromLengths(0, 8, 0, 0));
    return t;
}

Border chip(const LabelChip& l) {
    const auto colors = labelColors(l.color);
    Border b;
    b.Background(brush(colors.background));
    b.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
    b.Padding(ThicknessHelper::FromLengths(8, 2, 8, 2));
    auto t = text(l.name.empty() ? " " : l.name, 12);
    t.FontWeight(winrt::Microsoft::UI::Text::FontWeights::Bold());
    t.Foreground(brush(colors.foreground));
    b.Child(t);
    return b;
}

PersonPicture avatar(const std::string& name, const std::string& initials, double size) {
    PersonPicture p;
    p.Width(size);
    p.Height(size);
    p.DisplayName(hs(name));
    if (!initials.empty()) p.Initials(hs(initials));
    ToolTipService::SetToolTip(p, winrt::box_value(hs(name)));
    return p;
}

// Lays `items` out in rows no wider than `width` (WinUI has no stock wrap panel).
void wrapInto(StackPanel const& host, const std::vector<FrameworkElement>& items, double width, double spacing) {
    host.Children().Clear();
    host.Spacing(spacing);
    StackPanel row{nullptr};
    double used = 0;
    const double inf = std::numeric_limits<float>::infinity();
    for (const auto& item : items) {
        item.Measure({static_cast<float>(inf), static_cast<float>(inf)});
        const double w = item.DesiredSize().Width;
        if (!row || used + spacing + w > width) {
            row = hstack(spacing);
            host.Children().Append(row);
            used = 0;
        } else {
            used += spacing;
        }
        row.Children().Append(item);
        used += w;
    }
}

// A "+ Etiqueta" style button with a flyout holding a list.
Button pickerButton(const char* label, const char* name, Flyout const& flyout, ListView const& list) {
    Button b;
    accessibleName(b, name);
    auto content = hstack(4);
    SymbolIcon plus(Symbol::Add);
    content.Children().Append(plus);
    content.Children().Append(text(label));
    b.Content(content);
    list.SelectionMode(ListViewSelectionMode::None);
    list.IsItemClickEnabled(true);
    list.MaxHeight(320);
    list.Width(240);
    flyout.Content(list);
    b.Flyout(flyout);
    return b;
}

Grid sectionHeader(const std::string& title, UIElement const& action) {
    Grid g;
    g.ColumnDefinitions().Append(ColumnDefinition());
    ColumnDefinition right;
    right.Width(GridLengthHelper::Auto());
    g.ColumnDefinitions().Append(right);
    auto t = section(title);
    t.VerticalAlignment(VerticalAlignment::Center);
    g.Children().Append(t);
    Grid::SetColumn(action.as<FrameworkElement>(), 1);
    g.Children().Append(action);
    return g;
}

bool ctrlDown() {
    using winrt::Windows::UI::Core::CoreVirtualKeyStates;
    auto state = winrt::Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(VirtualKey::Control);
    return (state & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;
}

}  // namespace

CardPanel::CardPanel(std::shared_ptr<CardPanelVM> vm, std::function<void()> onClose)
    : vm_(std::move(vm)), onClose_(std::move(onClose)) {
    for (auto height : {GridLengthHelper::Auto(), GridLengthHelper::Auto(),
                        GridLengthHelper::FromValueAndType(1, GridUnitType::Star)}) {
        RowDefinition r;
        r.Height(height);
        root_.RowDefinitions().Append(r);
    }
    root_.Width(kPaneWidth);
    root_.Background(themeBrush(L"SolidBackgroundFillColorBaseBrush"));
    root_.BorderBrush(themeBrush(L"CardStrokeColorDefaultBrush"));
    root_.BorderThickness(ThicknessHelper::FromLengths(1, 0, 0, 0));

    // ---- header: title + close
    Grid header;
    header.Padding(ThicknessHelper::FromLengths(12, 12, 8, 4));
    header.ColumnSpacing(6);
    header.ColumnDefinitions().Append(ColumnDefinition());
    ColumnDefinition closeCol;
    closeCol.Width(GridLengthHelper::Auto());
    header.ColumnDefinitions().Append(closeCol);
    accessibleName(title_, "Título do card");
    title_.FontSize(20);
    title_.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
    title_.TextWrapping(TextWrapping::Wrap);
    title_.Background(brush({0, 0, 0, 0}));
    title_.BorderThickness(ThicknessHelper::FromUniformLength(0));
    title_.GotFocus([this](auto&&, auto&&) {
        editingTitle_ = true;
        titleBase_ = shownTitle_;  // what the user was looking at when they began
    });
    title_.LostFocus([this](auto&&, auto&&) {
        if (!editingTitle_) return;
        editingTitle_ = false;
        commitTitle();
    });
    title_.KeyDown([this](auto&&, Input::KeyRoutedEventArgs const& e) {
        if (e.Key() == VirtualKey::Enter) {
            e.Handled(true);
            editingTitle_ = false;
            commitTitle();
            root_.Focus(FocusState::Programmatic);
        } else if (e.Key() == VirtualKey::Escape) {
            e.Handled(true);
            editingTitle_ = false;
            title_.Text(hs(shownTitle_));
            root_.Focus(FocusState::Programmatic);
        }
    });
    header.Children().Append(title_);
    Button close;
    close.Content(SymbolIcon(Symbol::Cancel));
    close.VerticalAlignment(VerticalAlignment::Top);
    close.Background(brush({0, 0, 0, 0}));
    close.BorderThickness(ThicknessHelper::FromUniformLength(0));
    ToolTipService::SetToolTip(close, winrt::box_value(L"Fechar"));
    accessibleName(close, "Fechar");
    close.Click([this](auto&&, auto&&) { onClose_(); });
    Grid::SetColumn(close, 1);
    header.Children().Append(close);
    root_.Children().Append(header);
    root_.IsTabStop(true);  // so Enter/Escape in the title can move focus away from it

    banner_.Severity(InfoBarSeverity::Warning);
    banner_.IsClosable(false);
    banner_.Margin(ThicknessHelper::FromLengths(12, 0, 12, 4));
    auto dismiss = button("Dispensar");
    dismiss.Click([this](auto&&, auto&&) {
        remoteChangedWhileEditing_ = false;
        vm_->dismissNotices();
        updateNotice(vm_->state().get());
    });
    banner_.ActionButton(dismiss);
    Grid::SetRow(banner_, 1);
    root_.Children().Append(banner_);

    // ---- content
    content_ = vstack(8);
    content_.Padding(ThicknessHelper::FromLengths(16, 0, 16, 16));

    listName_.Opacity(0.7);
    content_.Children().Append(listName_);

    addLabelButton_ = pickerButton("Etiqueta", "Adicionar etiqueta", addLabelFlyout_, addLabelList_);
    addLabelList_.ItemClick([this](auto&&, ItemClickEventArgs const& e) {
        addLabelFlyout_.Hide();
        if (auto id = clickedId(e.ClickedItem()); !id.empty()) vm_->addLabel(id);
    });
    content_.Children().Append(sectionHeader("Etiquetas", addLabelButton_));
    labelsBox_ = vstack(6);
    content_.Children().Append(labelsBox_);

    addMemberButton_ = pickerButton("Membro", "Adicionar membro", addMemberFlyout_, addMemberList_);
    addMemberList_.ItemClick([this](auto&&, ItemClickEventArgs const& e) {
        addMemberFlyout_.Hide();
        if (auto id = clickedId(e.ClickedItem()); !id.empty()) vm_->addMember(id);
    });
    content_.Children().Append(sectionHeader("Membros", addMemberButton_));
    membersBox_ = vstack(4);
    content_.Children().Append(membersBox_);

    content_.Children().Append(section("Descrição"));
    description_.AcceptsReturn(true);
    description_.TextWrapping(TextWrapping::Wrap);
    description_.MinHeight(96);
    description_.MaxHeight(320);
    description_.PlaceholderText(L"Adicione uma descrição…");
    ScrollViewer::SetVerticalScrollBarVisibility(description_, ScrollBarVisibility::Auto);
    description_.TextChanged([this](auto&&, auto&&) { descriptionEdited(); });
    content_.Children().Append(description_);
    descButtons_ = hstack(6);
    descButtons_.HorizontalAlignment(HorizontalAlignment::Right);
    auto revert = button("Cancelar");
    revert.Click([this](auto&&, auto&&) { revertDescription(); });
    auto save = button("Salvar", true);
    save.Click([this](auto&&, auto&&) { saveDescription(); });
    descButtons_.Children().Append(revert);
    descButtons_.Children().Append(save);
    show(descButtons_, false);
    content_.Children().Append(descButtons_);

    extrasSpinner_ = hstack(8);
    ProgressRing ring;
    ring.IsActive(true);
    ring.Width(16);
    ring.Height(16);
    extrasSpinner_.Children().Append(ring);
    auto extrasText = text("Carregando checklists e anexos…");
    extrasText.Opacity(0.7);
    extrasSpinner_.Children().Append(extrasText);
    content_.Children().Append(extrasSpinner_);
    checklistsBox_ = vstack(4);
    content_.Children().Append(checklistsBox_);
    attachmentsBox_ = vstack(2);
    content_.Children().Append(attachmentsBox_);

    content_.Children().Append(section("Comentários"));
    comment_.AcceptsReturn(true);
    comment_.TextWrapping(TextWrapping::Wrap);
    comment_.MinHeight(56);
    comment_.MaxHeight(200);
    comment_.PlaceholderText(L"Escreva um comentário… (Ctrl+Enter envia)");
    comment_.TextChanged([this](auto&&, auto&&) { sendButton_.IsEnabled(!trimmed(fromBox(comment_.Text())).empty()); });
    comment_.PreviewKeyDown([this](auto&&, Input::KeyRoutedEventArgs const& e) {
        if (e.Key() == VirtualKey::Enter && ctrlDown()) {
            e.Handled(true);
            sendComment();
        }
    });
    content_.Children().Append(comment_);
    sendButton_ = button("Comentar", true);
    sendButton_.HorizontalAlignment(HorizontalAlignment::Right);
    sendButton_.IsEnabled(false);
    sendButton_.Click([this](auto&&, auto&&) { sendComment(); });
    content_.Children().Append(sendButton_);
    commentsBox_ = vstack(8);
    content_.Children().Append(commentsBox_);

    contentScroll_.Content(content_);
    contentScroll_.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    contentScroll_.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    Grid::SetRow(contentScroll_, 2);
    root_.Children().Append(contentScroll_);

    loading_.IsActive(true);
    loading_.Width(32);
    loading_.Height(32);
    loading_.HorizontalAlignment(HorizontalAlignment::Center);
    loading_.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetRow(loading_, 2);
    root_.Children().Append(loading_);

    conn_ = vm_->state().changed.connect([this](const CardPanelState& s) { update(s); });
    update(vm_->state().get());
}

CardPanel::~CardPanel() {
    vm_->state().changed.disconnect(conn_);
    // Closing the panel must not throw away what was typed: an unsent draft is saved.
    if (editingDesc_ && descriptionText() != shownDescription_) vm_->setDescription(descriptionText(), descBase_);
}

std::string CardPanel::descriptionText() const { return fromBox(description_.Text()); }

void CardPanel::setDescriptionText(const std::string& value) {
    settingText_ = true;
    description_.Text(hs(value));
    settingText_ = false;
}

void CardPanel::commitTitle() {
    const std::string value = trimmed(fromBox(title_.Text()));
    if (value.empty() || value == shownTitle_) {
        title_.Text(hs(shownTitle_));
        return;
    }
    vm_->setTitle(value, titleBase_.empty() ? shownTitle_ : titleBase_);
}

void CardPanel::descriptionEdited() {
    if (settingText_) return;
    const bool dirty = descriptionText() != shownDescription_;
    if (dirty && !editingDesc_) {
        editingDesc_ = true;
        descBase_ = shownDescription_;  // the text the user saw when they started typing
    }
    if (!dirty) editingDesc_ = false;
    show(descButtons_, dirty);
}

void CardPanel::saveDescription() {
    vm_->setDescription(descriptionText(), descBase_);
    editingDesc_ = false;
    remoteChangedWhileEditing_ = false;
    show(descButtons_, false);
    updateNotice(vm_->state().get());
}

void CardPanel::revertDescription() {
    setDescriptionText(shownDescription_);
    editingDesc_ = false;
    remoteChangedWhileEditing_ = false;
    show(descButtons_, false);
    updateNotice(vm_->state().get());
}

void CardPanel::sendComment() {
    const std::string value = trimmed(fromBox(comment_.Text()));
    if (value.empty()) return;
    vm_->addComment(value);
    comment_.Text(L"");
}

void CardPanel::update(const CardPanelState& s) {
    using Status = CardPanelState::Status;
    const bool loading = s.status == Status::Loading;
    show(loading_, loading);
    show(contentScroll_, !loading);
    if (loading) return;
    content_.IsHitTestVisible(s.status != Status::Removed);
    content_.Opacity(s.status == Status::Removed ? 0.5 : 1.0);
    title_.IsReadOnly(s.status == Status::Removed);

    updateNotice(s);

    if (s.title != shownTitle_) {
        shownTitle_ = s.title;
        if (!editingTitle_) title_.Text(hs(s.title));
    }
    listName_.Text(hs(s.listName.empty() ? "" : "na lista " + s.listName));
    show(listName_, !s.listName.empty());

    // never clobber what the user is typing; follow remote changes only when nothing is pending
    if (s.description != shownDescription_) {
        const bool editing = descriptionText() != shownDescription_;
        shownDescription_ = s.description;
        if (!editing) {
            setDescriptionText(s.description);
            show(descButtons_, false);
        } else {
            show(descButtons_, descriptionText() != shownDescription_);
            remoteChangedWhileEditing_ = true;  // saving now would overwrite their change
        }
    }

    updateLabels(s);
    updateMembers(s);
    updateComments(s);
    updateExtras(s);
}

void CardPanel::updateNotice(const CardPanelState& s) {
    if (remoteChangedWhileEditing_) {
        banner_.Message(L"A descrição foi alterada por outra pessoa enquanto você editava. Salvar vai sobrescrevê-la.");
        banner_.IsOpen(true);
        shownNotices_.clear();
        return;
    }
    if (s.notices == shownNotices_ && !s.notices.empty()) return;
    shownNotices_ = s.notices;
    if (s.notices.empty()) {
        banner_.IsOpen(false);
        return;
    }
    const PanelNotice& n = s.notices.front();
    std::string message;
    switch (n.kind) {
        case PanelNotice::Kind::RemovedRemotely:
            message = "Este card foi arquivado ou excluído por outra pessoa.";
            break;
        case PanelNotice::Kind::ConflictOverwritten:
            message = std::string("Você sobrescreveu uma alteração de outra pessoa ") +
                      (n.field == "name" ? "no título." : "na descrição.");
            break;
        case PanelNotice::Kind::WriteRejected:
            message = "O Trello recusou uma alteração; ela foi desfeita.";
            break;
    }
    banner_.Message(hs(message));
    banner_.IsOpen(true);
}

void CardPanel::updateLabels(const CardPanelState& s) {
    const bool appliedChanged = s.labels != shownLabels_;
    if (appliedChanged) {
        shownLabels_ = s.labels;
        std::vector<FrameworkElement> chips;
        for (const auto& l : s.labels) chips.push_back(chip(l));
        wrapInto(labelsBox_, chips, kInnerWidth, 6);
    }
    // the picker offers only labels not yet on the card, so it follows both lists
    if (appliedChanged || s.boardLabels != shownBoardLabels_ || !labelPickerBuilt_) {
        labelPickerBuilt_ = true;
        shownBoardLabels_ = s.boardLabels;
        addLabelList_.Items().Clear();
        for (const auto& l : s.boardLabels) {
            if (std::any_of(s.labels.begin(), s.labels.end(), [&](const LabelChip& x) { return x.id == l.id; })) continue;
            ListViewItem item;
            auto c = chip(l);
            c.HorizontalAlignment(HorizontalAlignment::Left);
            c.Tag(winrt::box_value(hs(l.id)));
            item.Content(c);
            accessibleName(item, l.name.empty() ? "Etiqueta sem nome (" + l.color + ")" : l.name);
            addLabelList_.Items().Append(item);
        }
        addLabelButton_.IsEnabled(addLabelList_.Items().Size() > 0);
    }
}

void CardPanel::updateMembers(const CardPanelState& s) {
    const bool appliedChanged = s.members != shownMembers_;
    if (appliedChanged) {
        shownMembers_ = s.members;
        std::vector<FrameworkElement> people;
        for (const auto& m : s.members) people.push_back(avatar(m.fullName, m.initials, 30));
        wrapInto(membersBox_, people, kInnerWidth, 4);
    }
    if (appliedChanged || s.boardMembers != shownBoardMembers_ || !memberPickerBuilt_) {
        memberPickerBuilt_ = true;
        shownBoardMembers_ = s.boardMembers;
        addMemberList_.Items().Clear();
        for (const auto& m : s.boardMembers) {
            if (std::any_of(s.members.begin(), s.members.end(), [&](const MemberChip& x) { return x.id == m.id; })) continue;
            ListViewItem item;
            auto row = hstack(8);
            row.Children().Append(avatar(m.fullName, m.initials, 24));
            auto name = text(m.fullName);
            name.VerticalAlignment(VerticalAlignment::Center);
            row.Children().Append(name);
            row.Tag(winrt::box_value(hs(m.id)));
            item.Content(row);
            accessibleName(item, m.fullName);
            addMemberList_.Items().Append(item);
        }
        addMemberButton_.IsEnabled(addMemberList_.Items().Size() > 0);
    }
}

void CardPanel::updateComments(const CardPanelState& s) {
    if (s.comments == shownComments_) return;
    shownComments_ = s.comments;
    commentsBox_.Children().Clear();
    for (const auto& c : s.comments) {
        Grid row;
        row.ColumnSpacing(8);
        ColumnDefinition who;
        who.Width(GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(who);
        row.ColumnDefinitions().Append(ColumnDefinition());
        auto picture = avatar(c.authorName.empty() ? "?" : c.authorName, c.authorInitials, 30);
        picture.VerticalAlignment(VerticalAlignment::Top);
        row.Children().Append(picture);

        Border bubble;
        bubble.Background(themeBrush(L"SolidBackgroundFillColorQuarternaryBrush"));
        bubble.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
        bubble.Padding(ThicknessHelper::FromLengths(10, 8, 10, 8));
        auto inner = vstack(2);
        auto head = hstack(8);
        auto name = text(c.authorName.empty() ? "Alguém" : c.authorName);
        name.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
        head.Children().Append(name);
        auto when = text(c.sending ? "enviando…" : formatDate(c.date), 12);
        when.Opacity(0.6);
        when.VerticalAlignment(VerticalAlignment::Center);
        head.Children().Append(when);
        inner.Children().Append(head);
        auto body = text(c.text, 0, true);
        body.IsTextSelectionEnabled(true);
        inner.Children().Append(body);
        bubble.Child(inner);
        Grid::SetColumn(bubble, 1);
        row.Children().Append(bubble);
        if (c.sending) row.Opacity(0.6);
        commentsBox_.Children().Append(row);
    }
}

void CardPanel::updateExtras(const CardPanelState& s) {
    if (s.extrasLoaded != shownExtrasLoaded_ || !s.extrasLoaded) {
        shownExtrasLoaded_ = s.extrasLoaded;
        show(extrasSpinner_, !s.extrasLoaded);
    }
    if (s.checklists != shownChecklists_) {
        shownChecklists_ = s.checklists;
        checklistsBox_.Children().Clear();
        for (const auto& list : s.checklists) {
            checklistsBox_.Children().Append(section(list.name));
            for (const auto& item : list.items) {
                CheckBox check;
                check.Content(winrt::box_value(hs(item.name)));
                check.IsChecked(item.done);
                check.IsEnabled(false);  // read-only in v1
                checklistsBox_.Children().Append(check);
            }
        }
    }
    if (s.attachments != shownAttachments_) {
        shownAttachments_ = s.attachments;
        attachmentsBox_.Children().Clear();
        if (!s.attachments.empty()) attachmentsBox_.Children().Append(section("Anexos"));
        for (const auto& a : s.attachments) {
            const std::string label = a.name.empty() ? a.url : a.name;
            // Teammates can attach anything: only web links are clickable (never file: or custom schemes).
            const bool web = a.url.rfind("https://", 0) == 0 || a.url.rfind("http://", 0) == 0;
            if (web) {
                HyperlinkButton link;
                link.Content(winrt::box_value(hs(label)));
                link.NavigateUri(winrt::Windows::Foundation::Uri(hs(a.url)));
                ToolTipService::SetToolTip(link, winrt::box_value(hs(a.url)));
                attachmentsBox_.Children().Append(link);
            } else {
                attachmentsBox_.Children().Append(text(label, 0, true));
            }
        }
    }
}

}  // namespace cardinal::win
