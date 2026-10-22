#include "pch.h"

#include "board_view.h"

#include <algorithm>

#include "style.h"
#include "win_util.h"

namespace cardinal::win {
namespace {

using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation;
using winrt::Windows::Foundation::IInspectable;

// Every card row is an empty Grid filled in code (ContainerContentChanging), so rows are recycled
// by the ListView without a XAML template per field.
DataTemplate cardTemplate() {
    static DataTemplate t = Markup::XamlReader::Load(
                                L"<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'>"
                                L"<Grid/></DataTemplate>")
                                .as<DataTemplate>();
    return t;
}

Style cardContainerStyle() {
    static Style s = [] {
        using winrt::Windows::UI::Xaml::Interop::TypeKind;
        winrt::Windows::UI::Xaml::Interop::TypeName type;
        type.Name = winrt::hstring(winrt::name_of<ListViewItem>());
        type.Kind = TypeKind::Metadata;
        Style style(type);
        auto set = [&](DependencyProperty const& p, IInspectable const& v) { style.Setters().Append(Setter(p, v)); };
        set(Control::PaddingProperty(), winrt::box_value(ThicknessHelper::FromUniformLength(0)));
        set(FrameworkElement::MarginProperty(), winrt::box_value(ThicknessHelper::FromLengths(0, 3, 0, 3)));
        set(FrameworkElement::MinHeightProperty(), winrt::box_value(0.0));
        set(Control::HorizontalContentAlignmentProperty(), winrt::box_value(HorizontalAlignment::Stretch));
        return style;
    }();
    return s;
}

void renderCard(Grid const& root, const CardRow& row) {
    root.Children().Clear();
    root.Background(themeBrush(L"SolidBackgroundFillColorQuarternaryBrush"));
    root.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
    root.Padding(ThicknessHelper::FromLengths(10, 8, 10, 8));
    root.BorderBrush(themeBrush(L"CardStrokeColorDefaultBrush"));
    root.BorderThickness(ThicknessHelper::FromUniformLength(1));

    auto body = vstack(6);
    if (!row.labels.empty()) {
        auto bars = hstack(4);
        for (const auto& l : row.labels) {
            Border bar;
            bar.Width(40);
            bar.Height(8);
            bar.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
            bar.Background(brush(labelColors(l.color).background));
            if (!l.name.empty()) ToolTipService::SetToolTip(bar, winrt::box_value(hs(l.name)));
            bars.Children().Append(bar);
        }
        body.Children().Append(bars);
    }
    auto title = text(row.title, 0, true);
    title.TextWrapping(TextWrapping::WrapWholeWords);
    body.Children().Append(title);

    if (row.commentCount > 0 || !row.members.empty()) {
        Grid footer;
        footer.ColumnDefinitions().Append(ColumnDefinition());
        ColumnDefinition right;
        right.Width(GridLengthHelper::Auto());
        footer.ColumnDefinitions().Append(right);
        if (row.commentCount > 0) {
            auto meta = hstack(4);
            meta.Opacity(0.7);
            FontIcon icon;
            icon.Glyph(L"");  // Message
            icon.FontSize(12);
            meta.Children().Append(icon);
            meta.Children().Append(text(std::to_string(row.commentCount), 12));
            meta.VerticalAlignment(VerticalAlignment::Center);
            footer.Children().Append(meta);
        }
        auto people = hstack(2);
        for (const auto& m : row.members) {
            PersonPicture p;
            p.Width(22);
            p.Height(22);
            p.DisplayName(hs(m.fullName));
            p.Initials(hs(m.initials));
            ToolTipService::SetToolTip(p, winrt::box_value(hs(m.fullName)));
            people.Children().Append(p);
        }
        Grid::SetColumn(people, 1);
        footer.Children().Append(people);
        body.Children().Append(footer);
    }
    root.Children().Append(body);
}

}  // namespace

struct BoardView::Column {
    std::string id;
    Border root;
    TextBlock title;
    TextBlock count;
    ListView list;
    StackPanel cold{nullptr};
    TextBlock coldCount;
    Button addButton{nullptr};
    TextBox addEntry;
    winrt::Windows::Foundation::Collections::IObservableVector<IInspectable> items =
        winrt::single_threaded_observable_vector<IInspectable>();
    std::vector<CardRow> shown;  // same order as `items`, which holds the card ids
};

BoardView::BoardView(std::shared_ptr<BoardVM> vm, std::function<void(const std::string&)> onOpenCard)
    : vm_(std::move(vm)), onOpenCard_(std::move(onOpenCard)) {
    LinearGradientBrush background;
    background.StartPoint({0, 0});
    background.EndPoint({1, 1});
    GradientStop a, b;
    a.Color(rgb(0x0B6FB3));
    a.Offset(0);
    b.Color(rgb(0x4F5FC4));
    b.Offset(1);
    background.GradientStops().Append(a);
    background.GradientStops().Append(b);
    root_.Background(background);

    box_.Orientation(Orientation::Horizontal);
    box_.Spacing(12);
    box_.Padding(ThicknessHelper::FromUniformLength(12));
    scroller_.Content(box_);
    scroller_.HorizontalScrollMode(ScrollMode::Enabled);
    scroller_.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
    scroller_.VerticalScrollMode(ScrollMode::Disabled);
    scroller_.VerticalScrollBarVisibility(ScrollBarVisibility::Disabled);
    root_.Children().Append(scroller_);

    // While the panel closes the board widens over several layouts; keep putting the offset back.
    scroller_.SizeChanged([this](auto&&, auto&&) {
        if (restoring_) scroller_.ChangeView(winrt::box_value(savedScroll_).as<winrt::Windows::Foundation::IReference<double>>(),
                                             nullptr, nullptr, true);
    });

    structureConn_ = vm_->structureChanged.connect([this] { rebuildStructure(); });
    columnConn_ = vm_->columnChanged.connect([this](const std::string& id) { updateColumn(id); });
    rebuildStructure();
}

BoardView::~BoardView() {
    if (restoreTimer_) restoreTimer_.Stop();
    vm_->structureChanged.disconnect(structureConn_);
    vm_->columnChanged.disconnect(columnConn_);
}

void BoardView::saveScroll() {
    if (scrollSaved_) return;  // a second card opened while the panel is already showing
    scrollSaved_ = true;
    restoring_ = false;
    savedScroll_ = scroller_.HorizontalOffset();
}

void BoardView::restoreScroll() {
    if (!scrollSaved_) return;
    scrollSaved_ = false;
    restoring_ = true;
    scroller_.ChangeView(winrt::box_value(savedScroll_).as<winrt::Windows::Foundation::IReference<double>>(),
                         nullptr, nullptr, true);
    if (!restoreTimer_) {
        restoreTimer_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
        restoreTimer_.IsRepeating(false);
        restoreTimer_.Tick([this](auto&&, auto&&) { restoring_ = false; });
    }
    restoreTimer_.Interval(std::chrono::milliseconds(600));
    restoreTimer_.Start();
}

BoardView::Column& BoardView::ensureColumn(const std::string& listId) {
    if (auto it = columns_.find(listId); it != columns_.end()) return *it->second;
    auto owned = std::make_unique<Column>();
    Column* c = owned.get();
    c->id = listId;

    c->root.Width(288);
    c->root.VerticalAlignment(VerticalAlignment::Top);
    c->root.Background(themeBrush(L"SolidBackgroundFillColorBaseBrush"));
    c->root.CornerRadius(CornerRadiusHelper::FromUniformRadius(12));
    c->root.Padding(ThicknessHelper::FromUniformLength(8));
    c->root.BorderThickness(ThicknessHelper::FromUniformLength(2));
    c->root.BorderBrush(brush({0, 0, 0, 0}));

    Grid layout;
    layout.RowSpacing(6);
    for (auto height : {GridLengthHelper::Auto(), GridLengthHelper::FromValueAndType(1, GridUnitType::Star),
                        GridLengthHelper::Auto()}) {
        RowDefinition r;
        r.Height(height);
        layout.RowDefinitions().Append(r);
    }

    Grid header;
    header.Padding(ThicknessHelper::FromLengths(6, 2, 6, 0));
    header.ColumnDefinitions().Append(ColumnDefinition());
    ColumnDefinition countCol;
    countCol.Width(GridLengthHelper::Auto());
    header.ColumnDefinitions().Append(countCol);
    c->title.FontWeight(winrt::Microsoft::UI::Text::FontWeights::Bold());
    c->title.TextTrimming(TextTrimming::CharacterEllipsis);
    c->count.Opacity(0.6);
    c->count.FontSize(12);
    c->count.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(c->count, 1);
    header.Children().Append(c->title);
    header.Children().Append(c->count);
    layout.Children().Append(header);

    c->list.ItemsSource(c->items);
    c->list.ItemTemplate(cardTemplate());
    c->list.ItemContainerStyle(cardContainerStyle());
    c->list.SelectionMode(ListViewSelectionMode::None);
    c->list.IsItemClickEnabled(true);
    c->list.CanDragItems(true);
    c->list.Padding(ThicknessHelper::FromUniformLength(0));
    c->list.ContainerContentChanging([c](ListViewBase const&, ContainerContentChangingEventArgs const& args) {
        if (args.InRecycleQueue()) return;
        const int index = args.ItemIndex();
        auto grid = args.ItemContainer().ContentTemplateRoot().try_as<Grid>();
        if (!grid || index < 0 || static_cast<size_t>(index) >= c->shown.size()) return;
        const CardRow& row = c->shown[static_cast<size_t>(index)];
        renderCard(grid, row);
        accessibleName(args.ItemContainer(), row.title);  // not the card id the item holds
        args.Handled(true);
    });
    c->list.ItemClick([this](auto&&, ItemClickEventArgs const& e) {
        onOpenCard_(winrt::to_string(winrt::unbox_value<winrt::hstring>(e.ClickedItem())));
    });
    c->list.DragItemsStarting([this](auto&&, DragItemsStartingEventArgs const& e) {
        dragging_ = winrt::to_string(winrt::unbox_value<winrt::hstring>(e.Items().GetAt(0)));
        e.Data().SetText(hs(dragging_));
        e.Data().RequestedOperation(DataPackageOperation::Move);
    });
    c->list.DragItemsCompleted([this](auto&&, auto&&) { dragging_.clear(); });
    Grid::SetRow(c->list, 1);
    layout.Children().Append(c->list);

    // A cold list: Trello's count, and a button to fetch its cards.
    c->cold = vstack(8);
    c->cold.Padding(ThicknessHelper::FromLengths(8, 24, 8, 24));
    c->coldCount.FontSize(28);
    c->coldCount.HorizontalAlignment(HorizontalAlignment::Center);
    c->cold.Children().Append(c->coldCount);
    auto coldText = text("cards ainda não carregados");
    coldText.Opacity(0.6);
    coldText.HorizontalAlignment(HorizontalAlignment::Center);
    c->cold.Children().Append(coldText);
    auto load = button("Carregar");
    load.HorizontalAlignment(HorizontalAlignment::Center);
    load.Click([this, listId](auto&&, auto&&) { vm_->expandList(listId); });
    c->cold.Children().Append(load);
    Grid::SetRow(c->cold, 1);
    layout.Children().Append(c->cold);

    // "+ Adicionar card": a button that turns into an entry
    Grid add;
    c->addButton = button("+ Adicionar card");
    c->addButton.HorizontalAlignment(HorizontalAlignment::Stretch);
    c->addButton.HorizontalContentAlignment(HorizontalAlignment::Left);
    c->addButton.Background(brush({0, 0, 0, 0}));
    c->addButton.BorderThickness(ThicknessHelper::FromUniformLength(0));
    c->addEntry.PlaceholderText(L"Título do card…");
    show(c->addEntry, false);
    c->addButton.Click([c](auto&&, auto&&) {
        show(c->addButton, false);
        show(c->addEntry, true);
        c->addEntry.Focus(FocusState::Programmatic);
    });
    c->addEntry.KeyDown([this, c, listId](auto&&, Input::KeyRoutedEventArgs const& e) {
        using winrt::Windows::System::VirtualKey;
        if (e.Key() == VirtualKey::Enter) {
            std::string title = str(c->addEntry.Text());
            if (title.empty()) return;
            vm_->createCard(listId, title);
            c->addEntry.Text(L"");  // stay open for the next one
            e.Handled(true);
        } else if (e.Key() == VirtualKey::Escape) {
            c->addEntry.Text(L"");
            show(c->addEntry, false);
            show(c->addButton, true);
            e.Handled(true);
        }
    });
    add.Children().Append(c->addButton);
    add.Children().Append(c->addEntry);
    Grid::SetRow(add, 2);
    layout.Children().Append(add);
    c->root.Child(layout);

    // Drops land on the column: the pointer's height picks the slot among the realized rows.
    c->root.AllowDrop(true);
    c->root.DragOver([this, c](auto&&, DragEventArgs const& e) {
        if (dragging_.empty()) return;
        e.AcceptedOperation(DataPackageOperation::Move);
        e.DragUIOverride().IsCaptionVisible(false);
        c->root.BorderBrush(themeBrush(L"AccentFillColorDefaultBrush"));
    });
    c->root.DragLeave([c](auto&&, auto&&) { c->root.BorderBrush(brush({0, 0, 0, 0})); });
    c->root.Drop([this, c](auto&&, DragEventArgs const& e) {
        c->root.BorderBrush(brush({0, 0, 0, 0}));
        if (dragging_.empty()) return;
        const auto at = e.GetPosition(c->list);
        int index = static_cast<int>(c->shown.size());
        int firstRealized = -1;
        for (uint32_t i = 0; i < c->items.Size(); ++i) {
            auto container = c->list.ContainerFromIndex(i).try_as<UIElement>();
            if (!container) continue;
            if (firstRealized < 0) firstRealized = static_cast<int>(i);
            const auto top = container.TransformToVisual(c->list).TransformPoint({0, 0});
            if (at.Y < top.Y + container.ActualSize().y / 2) {
                index = static_cast<int>(i);
                break;
            }
        }
        if (firstRealized > 0 && index == static_cast<int>(c->shown.size()) && at.Y < 0) index = firstRealized;
        dropCard(dragging_, c->id, index);
        dragging_.clear();
    });

    box_.Children().Append(c->root);
    columns_[listId] = std::move(owned);
    return *c;
}

void BoardView::updateColumn(const std::string& listId) {
    auto col = vm_->column(listId);
    if (!col) return;
    Column& c = ensureColumn(listId);
    c.title.Text(hs(col->name));
    c.count.Text(winrt::to_hstring(col->cardCount));
    c.coldCount.Text(winrt::to_hstring(col->cardCount));
    show(c.list, col->loaded);
    show(c.cold, !col->loaded);
    if (!col->loaded) return;

    // Keep unchanged rows (and their containers) and replace only the stretch that differs.
    const auto& next = col->cards;
    auto& shown = c.shown;
    size_t prefix = 0;
    while (prefix < shown.size() && prefix < next.size() && shown[prefix] == next[prefix]) ++prefix;
    size_t suffix = 0;
    while (suffix < shown.size() - prefix && suffix < next.size() - prefix &&
           shown[shown.size() - 1 - suffix] == next[next.size() - 1 - suffix])
        ++suffix;
    const size_t removeCount = shown.size() - prefix - suffix;
    const size_t addCount = next.size() - prefix - suffix;
    if (removeCount == 0 && addCount == 0) return;

    shown = next;  // before touching items: ContainerContentChanging reads it
    if (removeCount + addCount > 64) {
        std::vector<IInspectable> all;
        all.reserve(next.size());
        for (const auto& r : next) all.push_back(winrt::box_value(hs(r.id)));
        c.items.ReplaceAll(all);
        return;
    }
    for (size_t i = 0; i < removeCount; ++i) c.items.RemoveAt(static_cast<uint32_t>(prefix));
    for (size_t i = 0; i < addCount; ++i)
        c.items.InsertAt(static_cast<uint32_t>(prefix + i), winrt::box_value(hs(next[prefix + i].id)));
    // Rows inside the changed stretch that kept their id still need repainting.
    for (size_t i = prefix; i < prefix + addCount; ++i)
        if (auto container = c.list.ContainerFromIndex(static_cast<uint32_t>(i)).try_as<ListViewItem>())
            if (auto grid = container.ContentTemplateRoot().try_as<Grid>()) renderCard(grid, shown[i]);
}

void BoardView::rebuildStructure() {
    std::vector<std::string> order;
    for (const auto& col : vm_->columns()) order.push_back(col->id);

    for (auto it = columns_.begin(); it != columns_.end();) {
        if (std::find(order.begin(), order.end(), it->first) == order.end()) {
            uint32_t index = 0;
            if (box_.Children().IndexOf(it->second->root, index)) box_.Children().RemoveAt(index);
            it = columns_.erase(it);
        } else {
            ++it;
        }
    }
    for (size_t i = 0; i < order.size(); ++i) {
        updateColumn(order[i]);
        auto& root = columns_[order[i]]->root;
        uint32_t at = 0;
        if (box_.Children().IndexOf(root, at) && at != i) {
            box_.Children().RemoveAt(at);
            box_.Children().InsertAt(static_cast<uint32_t>(i), root);
        }
    }
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

}  // namespace cardinal::win
