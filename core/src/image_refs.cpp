#include "cardinal/image_refs.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace cardinal {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool isWeb(std::string_view url) {
    const std::string l = lower(url.substr(0, 8));
    return l.rfind("https://", 0) == 0 || l.rfind("http://", 0) == 0;
}

bool hasImageExtension(std::string_view name) {
    static constexpr std::array<std::string_view, 6> kExt = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"};
    const std::string l = lower(name);
    return std::any_of(kExt.begin(), kExt.end(), [&](std::string_view e) {
        return l.size() > e.size() && l.compare(l.size() - e.size(), e.size(), e) == 0;
    });
}

void add(std::vector<std::string>& out, std::string url) {
    if (url.empty() || !isWeb(url)) return;
    if (std::find(out.begin(), out.end(), url) == out.end()) out.push_back(std::move(url));
}

// The URL inside `(...)` of a markdown image starting at `open` (just past '('). Accepts `<url>`
// and an optional quoted title after it.
// `*close` is set to the index just past the closing ')' (or text.size() if there is none).
std::string markdownTarget(std::string_view text, size_t open, size_t* close = nullptr) {
    size_t i = open;
    auto closeAfter = [&](size_t from) {
        if (!close) return;
        const size_t paren = text.find(')', from);
        *close = paren == std::string_view::npos ? text.size() : paren + 1;
    };
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i < text.size() && text[i] == '<') {
        const size_t end = text.find('>', i + 1);
        closeAfter(end == std::string_view::npos ? text.size() : end);
        return end == std::string_view::npos ? "" : std::string(text.substr(i + 1, end - i - 1));
    }
    size_t end = i;
    int depth = 0;  // URLs may hold balanced parentheses
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) {
        if (text[end] == '(') ++depth;
        if (text[end] == ')') {
            if (depth == 0) break;
            --depth;
        }
        ++end;
    }
    closeAfter(end);  // past an optional "title"
    return std::string(text.substr(i, end - i));
}

}  // namespace

bool isImageUrl(std::string_view url) {
    const size_t cut = url.find_first_of("?#");
    return hasImageExtension(url.substr(0, cut));
}

bool isImageAttachment(const Attachment& a) {
    const std::string mime = lower(a.mimeType);
    if (!mime.empty()) return mime.rfind("image/", 0) == 0 && mime.find("svg") == std::string::npos;
    return hasImageExtension(a.name) || isImageUrl(a.url);
}

std::vector<std::string> imageUrlsIn(std::string_view text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        const size_t img = text.find("![", i);
        const size_t web = std::min(text.find("http://", i), text.find("https://", i));
        if (img == std::string_view::npos && web == std::string_view::npos) break;
        if (img != std::string_view::npos && (web == std::string_view::npos || img < web)) {
            // ![alt](target): the alt text may hold brackets, the link must follow `]` directly.
            const size_t close = text.find("](", img + 2);
            const size_t line = text.find('\n', img + 2);
            if (close == std::string_view::npos || (line != std::string_view::npos && line < close)) {
                i = img + 2;
                continue;
            }
            add(out, markdownTarget(text, close + 2));
            i = close + 2;
            continue;
        }
        size_t end = web;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) &&
               std::string_view(")]>\"'<").find(text[end]) == std::string_view::npos)
            ++end;
        std::string_view url = text.substr(web, end - web);
        while (!url.empty() && std::string_view(".,;:!?").find(url.back()) != std::string_view::npos)
            url.remove_suffix(1);
        if (isImageUrl(url)) add(out, std::string(url));
        i = end;
    }
    return out;
}

std::string withoutMarkdownImages(std::string_view text) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        const size_t img = text.find("![", i);
        if (img == std::string_view::npos) break;
        const size_t close = text.find("](", img + 2);
        const size_t line = text.find('\n', img + 2);
        if (close == std::string_view::npos || (line != std::string_view::npos && line < close)) {
            out.append(text.substr(i, img + 2 - i));
            i = img + 2;
            continue;
        }
        size_t end = 0;
        markdownTarget(text, close + 2, &end);
        out.append(text.substr(i, img - i));
        i = end;
    }
    out.append(text.substr(std::min(i, text.size())));
    // collapse the empty lines the pictures leave, and trim
    std::string clean;
    int newlines = 0;
    for (char c : out) {
        if (c == '\n') {
            if (++newlines > 2) continue;
        } else if (c != ' ' && c != '\t' && c != '\r') {
            newlines = 0;
        }
        clean += c;
    }
    const size_t b = clean.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return clean.substr(b, clean.find_last_not_of(" \t\r\n") - b + 1);
}

}  // namespace cardinal
