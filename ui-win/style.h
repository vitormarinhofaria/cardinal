#pragma once

#include <string>

// Colors shared by the board and the card panel. Label colors match ui-gtk/src/style.h (Trello's
// palette); surfaces come from the WinUI theme so light and dark both work.
namespace cardinal::win {

struct LabelColors {
    winrt::Windows::UI::Color background;
    winrt::Windows::UI::Color foreground;
};

inline winrt::Windows::UI::Color rgb(uint32_t v) {
    return {0xFF, static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
}

inline LabelColors labelColors(const std::string& color) {
    const auto white = rgb(0xFFFFFF), ink = rgb(0x1D2125);
    struct Entry { const char* name; uint32_t bg; bool dark; };
    static const Entry table[] = {
        {"green", 0x61BD4F, false},  {"yellow", 0xC9AD00, true},   {"orange", 0xE88D00, false},
        {"red", 0xEB5A46, false},    {"purple", 0xC377E0, false},  {"blue", 0x0079BF, false},
        {"sky", 0x00A2C0, false},    {"lime", 0x2FBF7A, false},    {"pink", 0xE050A8, false},
        {"black", 0x344563, false},
        {"green_light", 0xBAF3DB, true},  {"green_dark", 0x1F845A, false},
        {"yellow_light", 0xF8E6A0, true}, {"yellow_dark", 0x946F00, false},
        {"orange_light", 0xFEDEC8, true}, {"orange_dark", 0xC25100, false},
        {"red_light", 0xFFD5D2, true},    {"red_dark", 0xC9372C, false},
        {"purple_light", 0xDFD8FD, true}, {"purple_dark", 0x6E5DC6, false},
        {"blue_light", 0xCCE0FF, true},   {"blue_dark", 0x0C66E4, false},
        {"sky_light", 0xC6EDFB, true},    {"sky_dark", 0x227D9B, false},
        {"lime_light", 0xD3F1A7, true},   {"lime_dark", 0x5B7F24, false},
        {"pink_light", 0xFDD0EC, true},   {"pink_dark", 0xAE4787, false},
        {"black_light", 0xDCDFE4, true},  {"black_dark", 0x44546F, false},
    };
    for (const auto& e : table)
        if (color == e.name) return {rgb(e.bg), e.dark ? ink : white};
    return {rgb(0x8590A2), white};  // no color
}

inline winrt::Microsoft::UI::Xaml::Media::SolidColorBrush brush(winrt::Windows::UI::Color c) {
    return winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(c);
}

// A theme brush by its resource key, e.g. L"SolidBackgroundFillColorBaseBrush".
inline winrt::Microsoft::UI::Xaml::Media::Brush themeBrush(const wchar_t* key) {
    return winrt::Microsoft::UI::Xaml::Application::Current()
        .Resources()
        .Lookup(winrt::box_value(key))
        .as<winrt::Microsoft::UI::Xaml::Media::Brush>();
}

}  // namespace cardinal::win
