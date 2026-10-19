#pragma once

#include <string>
#include <string_view>

// Small helpers for building WinUI from C++ code (the UI is built in code, like ui-gtk, so each
// page is a plain C++ class and needs no IDL/XAML of its own).
namespace cardinal::win {

inline std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline winrt::hstring hs(std::string_view s) { return winrt::hstring(widen(s)); }

inline std::string str(winrt::hstring const& h) { return winrt::to_string(h); }

namespace ux = winrt::Microsoft::UI::Xaml;
namespace uc = winrt::Microsoft::UI::Xaml::Controls;

inline uc::TextBlock text(std::string_view s, double size = 0, bool wrap = false) {
    uc::TextBlock t;
    t.Text(hs(s));
    if (size > 0) t.FontSize(size);
    if (wrap) t.TextWrapping(ux::TextWrapping::Wrap);
    return t;
}

inline uc::Button button(std::string_view label, bool accent = false) {
    uc::Button b;
    b.Content(winrt::box_value(hs(label)));
    if (accent)
        b.Style(ux::Application::Current().Resources().Lookup(winrt::box_value(L"AccentButtonStyle"))
                    .as<ux::Style>());
    return b;
}

inline uc::StackPanel vstack(double spacing = 0) {
    uc::StackPanel p;
    p.Orientation(uc::Orientation::Vertical);
    p.Spacing(spacing);
    return p;
}

inline uc::StackPanel hstack(double spacing = 0) {
    uc::StackPanel p;
    p.Orientation(uc::Orientation::Horizontal);
    p.Spacing(spacing);
    return p;
}

// What a screen reader announces for an element whose content is an icon or a composite.
inline void accessibleName(ux::DependencyObject const& e, std::string_view name) {
    ux::Automation::AutomationProperties::SetName(e, hs(name));
}

// A ListView of ListViewItems built in code hands ItemClick the item's *content*, so pickers put
// the id in the content's Tag. Empty when there is none.
inline std::string clickedId(winrt::Windows::Foundation::IInspectable const& clicked) {
    auto element = clicked.try_as<ux::FrameworkElement>();
    if (!element) return {};
    auto tag = element.Tag().try_as<winrt::hstring>();
    return tag ? winrt::to_string(*tag) : std::string();
}

inline void show(ux::UIElement const& e, bool visible) {
    e.Visibility(visible ? ux::Visibility::Visible : ux::Visibility::Collapsed);
}

}  // namespace cardinal::win
