#pragma once

#include <string>
#include <string_view>
#include <vector>

// Trello's markdown (CommonMark + GitHub tables, strikethrough, task lists, autolinks) parsed into
// a flat list of blocks a UI can lay out without knowing markdown. Raw HTML is shown as text.
namespace cardinal::md {

struct Span {
    std::string text;
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
    bool lineBreak = false;     // a line break; `text` is empty
    std::string link;           // where it points, empty when it is not a link
    std::string cardShortLink;  // the link points to a Trello card (trello.com/c/<shortLink>)
    bool cardName = false;      // show that card's name instead of `text` (the link is a bare URL)
    bool operator==(const Span&) const = default;
};

using Inline = std::vector<Span>;

struct Block {
    enum class Kind { Paragraph, Heading, Code, Rule, Image, Table };
    Kind kind = Kind::Paragraph;
    int level = 0;         // heading level, 1-6
    int quoteDepth = 0;    // nested `>` quotes it sits in
    int listDepth = 0;     // nested list items it sits in
    std::string marker;    // "•", "3.", "☐", "☑": set on the first block of a list item only
    Inline spans;          // Paragraph, Heading
    std::string text;      // Code: the code; Image: the alt text
    std::string url;       // Image
    std::vector<std::vector<Inline>> rows;  // Table: rows of cells, the header row first
    bool operator==(const Block&) const = default;
};

std::vector<Block> parse(std::string_view text);

// "abc123" for https://trello.com/c/abc123/12-some-title, empty for any other URL.
std::string cardShortLinkOf(std::string_view url);

// The cards `text` links to, in order of appearance, without repeats.
std::vector<std::string> cardShortLinksIn(std::string_view text);

}  // namespace cardinal::md
