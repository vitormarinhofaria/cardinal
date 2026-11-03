#include "cardinal/markdown.h"

#include <md4c.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <utility>

#include "cardinal/image_refs.h"

namespace cardinal::md {
namespace {

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// "&amp;", "&#233;", "&#xE9;": the common ones; anything else stays as written.
std::string decodeEntity(std::string_view e) {
    if (e.size() > 3 && e[1] == '#') {
        const bool hex = e[2] == 'x' || e[2] == 'X';
        uint32_t cp = 0;
        for (size_t i = hex ? 3 : 2; i + 1 < e.size(); ++i) {
            const char c = e[i];
            const int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                          : hex && std::isxdigit(static_cast<unsigned char>(c))
                              ? std::tolower(static_cast<unsigned char>(c)) - 'a' + 10
                              : -1;
            if (d < 0) return std::string(e);
            cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(d);
            if (cp > 0x10FFFF) cp = 0xFFFD;
        }
        std::string out;
        appendUtf8(out, cp);
        return out;
    }
    static constexpr std::pair<std::string_view, std::string_view> kNamed[] = {
        {"&amp;", "&"},   {"&lt;", "<"},     {"&gt;", ">"},    {"&quot;", "\""},
        {"&apos;", "'"},  {"&nbsp;", " "}, {"&copy;", "©"}, {"&mdash;", "—"},
        {"&ndash;", "–"}, {"&hellip;", "…"},
    };
    for (const auto& [name, value] : kNamed)
        if (e == name) return std::string(value);
    return std::string(e);
}

std::string attr(const MD_ATTRIBUTE& a) {
    std::string out;
    if (!a.text) return out;
    for (int i = 0; a.substr_offsets[i] < a.size; ++i) {
        const std::string_view piece(a.text + a.substr_offsets[i], a.substr_offsets[i + 1] - a.substr_offsets[i]);
        if (a.substr_types[i] == MD_TEXT_ENTITY)
            out += decodeEntity(piece);
        else if (a.substr_types[i] != MD_TEXT_NULLCHAR)
            out += piece;
    }
    return out;
}

class Builder {
public:
    std::vector<Block> blocks;

    void enterBlock(MD_BLOCKTYPE type, void* detail) {
        switch (type) {
            case MD_BLOCK_QUOTE:
                close();
                ++quoteDepth_;
                break;
            case MD_BLOCK_UL:
                close();
                lists_.push_back({false, 0});
                break;
            case MD_BLOCK_OL:
                close();
                lists_.push_back({true, static_cast<int>(static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start)});
                break;
            case MD_BLOCK_LI: {
                close();
                ++listDepth_;
                const auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
                if (li->is_task)
                    marker_ = (li->task_mark == ' ') ? "☐" : "☑";
                else if (!lists_.empty() && lists_.back().ordered)
                    marker_ = std::to_string(lists_.back().next++) + ".";
                else
                    marker_ = "•";
                break;
            }
            case MD_BLOCK_HR:
                close();
                push(Block::Kind::Rule);
                break;
            case MD_BLOCK_H:
                close();
                push(Block::Kind::Heading).level = static_cast<int>(static_cast<MD_BLOCK_H_DETAIL*>(detail)->level);
                open_ = true;
                break;
            case MD_BLOCK_CODE:
                close();
                push(Block::Kind::Code);
                code_ = true;
                break;
            case MD_BLOCK_P:
                close();
                push(Block::Kind::Paragraph);
                open_ = true;
                break;
            case MD_BLOCK_TABLE:
                close();
                table_ = Block{};
                table_->kind = Block::Kind::Table;
                break;
            case MD_BLOCK_TR:
                if (table_) table_->rows.emplace_back();
                break;
            case MD_BLOCK_TH:
            case MD_BLOCK_TD:
                if (table_ && !table_->rows.empty()) {
                    table_->rows.back().emplace_back();
                    cell_ = &table_->rows.back().back();
                }
                break;
            default:
                break;
        }
    }

    void leaveBlock(MD_BLOCKTYPE type) {
        switch (type) {
            case MD_BLOCK_QUOTE:
                close();
                --quoteDepth_;
                break;
            case MD_BLOCK_UL:
            case MD_BLOCK_OL:
                close();
                if (!lists_.empty()) lists_.pop_back();
                break;
            case MD_BLOCK_LI:
                close();
                if (!marker_.empty()) push(Block::Kind::Paragraph);  // an empty item still shows its bullet
                --listDepth_;
                marker_.clear();
                break;
            case MD_BLOCK_H:
            case MD_BLOCK_P:
                close();
                break;
            case MD_BLOCK_CODE: {
                code_ = false;
                std::string& t = blocks.back().text;
                if (!t.empty() && t.back() == '\n') t.pop_back();
                break;
            }
            case MD_BLOCK_TH:
            case MD_BLOCK_TD:
                if (cell_) trimBreaks(*cell_);
                cell_ = nullptr;
                break;
            case MD_BLOCK_TABLE:
                if (table_) {
                    Block b = std::move(*table_);
                    table_.reset();
                    stamp(b);
                    blocks.push_back(std::move(b));
                }
                break;
            default:
                break;
        }
    }

    void enterSpan(MD_SPANTYPE type, void* detail) {
        switch (type) {
            case MD_SPAN_EM: ++italic_; break;
            case MD_SPAN_STRONG: ++bold_; break;
            case MD_SPAN_DEL: ++strike_; break;
            case MD_SPAN_CODE: ++code_span_; break;
            case MD_SPAN_A: {
                const auto* a = static_cast<MD_SPAN_A_DETAIL*>(detail);
                link_ = attr(a->href);
                if (link_.rfind("www.", 0) == 0) link_ = "http://" + link_;
                linkTitle_ = attr(a->title);
                linkAuto_ = a->is_autolink != 0;
                linkInline_ = &target();
                linkStart_ = linkInline_->size();
                break;
            }
            case MD_SPAN_IMG:
                if (img_++ == 0) {
                    imgSrc_ = attr(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src);
                    imgAlt_.clear();
                }
                break;
            default:
                break;
        }
    }

    void leaveSpan(MD_SPANTYPE type) {
        switch (type) {
            case MD_SPAN_EM: --italic_; break;
            case MD_SPAN_STRONG: --bold_; break;
            case MD_SPAN_DEL: --strike_; break;
            case MD_SPAN_CODE: --code_span_; break;
            case MD_SPAN_A:
                finishLink();
                break;
            case MD_SPAN_IMG:
                if (--img_ == 0) image();
                break;
            default:
                break;
        }
    }

    void text(MD_TEXTTYPE type, std::string_view t) {
        if (code_ && type == MD_TEXT_CODE) {
            blocks.back().text += t;
            return;
        }
        const bool lineBreak = type == MD_TEXT_BR || type == MD_TEXT_SOFTBR;
        if (img_) {
            imgAlt_ += lineBreak ? std::string(" ") : type == MD_TEXT_ENTITY ? decodeEntity(t) : std::string(t);
            return;
        }
        if (lineBreak) {
            // Trello shows each line of a paragraph on its own line, not joined like CommonMark.
            if (cell_) {
                add(" ");
            } else {
                Span s;
                s.lineBreak = true;
                s.link = link_;
                target().push_back(std::move(s));
            }
            return;
        }
        if (type == MD_TEXT_NULLCHAR) return add("�");
        add(type == MD_TEXT_ENTITY ? decodeEntity(t) : std::string(t));
    }

    void finish() {
        close();
        if (table_) {
            blocks.push_back(std::move(*table_));
            table_.reset();
        }
    }

private:
    struct ListCtx {
        bool ordered;
        int next;
    };

    // Depths and, once per list item, its marker.
    void stamp(Block& b) {
        b.quoteDepth = quoteDepth_;
        b.listDepth = listDepth_;
        b.marker = std::exchange(marker_, "");
    }

    Block& push(Block::Kind kind) {
        Block b;
        b.kind = kind;
        stamp(b);
        blocks.push_back(std::move(b));
        return blocks.back();
    }

    // Where text goes now: the table cell, the open paragraph, or a new one (tight list items
    // hold their text directly, with no paragraph around it).
    Inline& target() {
        if (cell_) return *cell_;
        if (!open_) {
            push(Block::Kind::Paragraph);
            open_ = true;
        }
        return blocks.back().spans;
    }

    void add(std::string text) {
        Span s;
        s.text = std::move(text);
        s.bold = bold_ > 0;
        s.italic = italic_ > 0;
        s.strike = strike_ > 0;
        s.code = code_span_ > 0;
        s.link = link_;
        Inline& in = target();
        if (!in.empty()) {
            Span& last = in.back();
            if (!last.lineBreak && last.bold == s.bold && last.italic == s.italic && last.strike == s.strike &&
                last.code == s.code && last.link == s.link && last.cardShortLink.empty()) {
                last.text += s.text;
                return;
            }
        }
        in.push_back(std::move(s));
    }

    static void trimBreaks(Inline& in) {
        while (!in.empty() && in.back().lineBreak) in.pop_back();
        while (!in.empty() && in.front().lineBreak) in.erase(in.begin());
    }

    void finishLink() {
        Inline* in = linkInline_;
        const std::string href = std::exchange(link_, "");
        linkInline_ = nullptr;
        if (!in || linkStart_ > in->size()) return;
        const std::string card = cardShortLinkOf(href);
        if (card.empty()) return;
        std::string shown;
        for (size_t i = linkStart_; i < in->size(); ++i) shown += (*in)[i].text;
        const bool bare = linkAuto_ || shown == href || linkTitle_.find("smartCard") != std::string::npos;
        if (bare) {
            in->erase(in->begin() + static_cast<std::ptrdiff_t>(linkStart_), in->end());
            Span s;
            s.text = href;
            s.link = href;
            s.cardShortLink = card;
            s.cardName = true;
            in->push_back(std::move(s));
        } else {
            for (size_t i = linkStart_; i < in->size(); ++i) (*in)[i].cardShortLink = card;
        }
    }

    void image() {
        if (cell_ || link_.size()) {
            // inside a table cell or a link: a link to the picture, keeps the layout
            Span s;
            s.text = imgAlt_.empty() ? imgSrc_ : imgAlt_;
            s.link = link_.empty() ? imgSrc_ : link_;
            target().push_back(std::move(s));
            return;
        }
        close();
        Block& b = push(Block::Kind::Image);
        b.url = imgSrc_;
        b.text = imgAlt_;
    }

    // Done with the open paragraph or heading: trim it, and a paragraph that is only a bare
    // link to a picture shows the picture.
    void close() {
        if (!open_) return;
        open_ = false;
        Block& b = blocks.back();
        trimBreaks(b.spans);
        if (b.kind != Block::Kind::Paragraph) return;
        if (b.spans.size() == 1 && b.spans[0].cardShortLink.empty() && b.spans[0].text == b.spans[0].link &&
            isImageUrl(b.spans[0].link)) {
            b.kind = Block::Kind::Image;
            b.url = b.spans[0].link;
            b.spans.clear();
            return;
        }
        if (b.spans.empty() && b.marker.empty()) blocks.pop_back();
    }

    int quoteDepth_ = 0;
    int listDepth_ = 0;
    std::vector<ListCtx> lists_;
    std::string marker_;
    bool open_ = false;
    bool code_ = false;
    std::optional<Block> table_;
    Inline* cell_ = nullptr;

    int bold_ = 0, italic_ = 0, strike_ = 0, code_span_ = 0;
    std::string link_, linkTitle_;
    bool linkAuto_ = false;
    Inline* linkInline_ = nullptr;
    size_t linkStart_ = 0;

    int img_ = 0;
    std::string imgSrc_, imgAlt_;
};

Builder& self(void* u) { return *static_cast<Builder*>(u); }

}  // namespace

std::vector<Block> parse(std::string_view text) {
    MD_PARSER p{};
    p.abi_version = 0;
    p.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEURLAUTOLINKS |
              MD_FLAG_PERMISSIVEWWWAUTOLINKS | MD_FLAG_NOHTML;
    p.enter_block = [](MD_BLOCKTYPE t, void* d, void* u) { return self(u).enterBlock(t, d), 0; };
    p.leave_block = [](MD_BLOCKTYPE t, void*, void* u) { return self(u).leaveBlock(t), 0; };
    p.enter_span = [](MD_SPANTYPE t, void* d, void* u) { return self(u).enterSpan(t, d), 0; };
    p.leave_span = [](MD_SPANTYPE t, void*, void* u) { return self(u).leaveSpan(t), 0; };
    p.text = [](MD_TEXTTYPE t, const MD_CHAR* s, MD_SIZE n, void* u) {
        return self(u).text(t, std::string_view(s, n)), 0;
    };
    Builder b;
    md_parse(text.data(), static_cast<MD_SIZE>(text.size()), &p, &b);
    b.finish();
    return std::move(b.blocks);
}

std::string cardShortLinkOf(std::string_view url) {
    auto eat = [&](std::string_view prefix) {
        if (url.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(url[i])) != prefix[i]) return false;
        url.remove_prefix(prefix.size());
        return true;
    };
    if (!eat("https://") && !eat("http://")) return "";
    eat("www.");
    if (!eat("trello.com/c/")) return "";
    size_t n = 0;
    while (n < url.size() && std::isalnum(static_cast<unsigned char>(url[n]))) ++n;
    if (n == 0 || (n < url.size() && std::string_view("/?#").find(url[n]) == std::string_view::npos)) return "";
    return std::string(url.substr(0, n));
}

std::vector<std::string> cardShortLinksIn(std::string_view text) {
    std::vector<std::string> out;
    auto take = [&](const Inline& in) {
        for (const auto& s : in)
            if (!s.cardShortLink.empty() && std::find(out.begin(), out.end(), s.cardShortLink) == out.end())
                out.push_back(s.cardShortLink);
    };
    if (text.find("trello.com/c/") == std::string_view::npos) return out;  // the common case: no parse
    for (const auto& b : parse(text)) {
        take(b.spans);
        for (const auto& row : b.rows)
            for (const auto& cell : row) take(cell);
    }
    return out;
}

}  // namespace cardinal::md
