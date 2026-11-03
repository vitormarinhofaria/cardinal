#include <gtest/gtest.h>

#include "cardinal/markdown.h"

using namespace cardinal::md;
using Kind = Block::Kind;

namespace {

std::string plain(const Inline& in) {
    std::string out;
    for (const auto& s : in) out += s.lineBreak ? "\n" : s.text;
    return out;
}

}  // namespace

TEST(Markdown, EmptyTextHasNoBlocks) {
    EXPECT_TRUE(parse("").empty());
    EXPECT_TRUE(parse("  \n\n ").empty());
}

TEST(Markdown, ParagraphsKeepTheirLineBreaksLikeTrello) {
    auto b = parse("primeira linha\nsegunda\n\noutro parágrafo");
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0].kind, Kind::Paragraph);
    EXPECT_EQ(plain(b[0].spans), "primeira linha\nsegunda");
    EXPECT_EQ(plain(b[1].spans), "outro parágrafo");
}

TEST(Markdown, Emphasis) {
    auto b = parse("a **negrito** _itálico_ ~~riscado~~ `código` ***ambos***");
    ASSERT_EQ(b.size(), 1u);
    const auto& s = b[0].spans;
    auto find = [&](const std::string& t) -> const Span& {
        for (const auto& x : s)
            if (x.text == t) return x;
        ADD_FAILURE() << "no span " << t;
        return s.front();
    };
    EXPECT_TRUE(find("negrito").bold);
    EXPECT_TRUE(find("itálico").italic);
    EXPECT_TRUE(find("riscado").strike);
    EXPECT_TRUE(find("código").code);
    EXPECT_TRUE(find("ambos").bold && find("ambos").italic);
    EXPECT_FALSE(find("a ").bold);
}

TEST(Markdown, HeadingsRulesAndCode) {
    auto b = parse("# Título\n\n### Sub\n\n---\n\n```cpp\nint x = 1 < 2;\n```\n");
    ASSERT_EQ(b.size(), 4u);
    EXPECT_EQ(b[0].kind, Kind::Heading);
    EXPECT_EQ(b[0].level, 1);
    EXPECT_EQ(plain(b[0].spans), "Título");
    EXPECT_EQ(b[1].level, 3);
    EXPECT_EQ(b[2].kind, Kind::Rule);
    EXPECT_EQ(b[3].kind, Kind::Code);
    EXPECT_EQ(b[3].text, "int x = 1 < 2;");
}

TEST(Markdown, ListsNestAndNumberAndShowTasks) {
    auto b = parse("- um\n- dois\n  1. a\n  2. b\n- [x] feito\n- [ ] pendente\n\n3. três\n4. quatro");
    ASSERT_EQ(b.size(), 8u);
    EXPECT_EQ(b[0].marker, "•");
    EXPECT_EQ(b[0].listDepth, 1);
    EXPECT_EQ(plain(b[0].spans), "um");
    EXPECT_EQ(b[2].marker, "1.");
    EXPECT_EQ(b[2].listDepth, 2);
    EXPECT_EQ(b[3].marker, "2.");
    EXPECT_EQ(b[4].marker, "☑");
    EXPECT_EQ(plain(b[4].spans), "feito");
    EXPECT_EQ(b[5].marker, "☐");
    EXPECT_EQ(b[6].marker, "3.");  // an ordered list keeps its start number
    EXPECT_EQ(b[7].marker, "4.");
}

TEST(Markdown, LooseListItemsMarkOnlyTheirFirstParagraph) {
    auto b = parse("- primeiro\n\n  continua\n\n- segundo");
    ASSERT_EQ(b.size(), 3u);
    EXPECT_EQ(b[0].marker, "•");
    EXPECT_EQ(b[1].marker, "");
    EXPECT_EQ(b[1].listDepth, 1);
    EXPECT_EQ(b[2].marker, "•");
}

TEST(Markdown, Quotes) {
    auto b = parse("> citação\n> > dentro\n\nfora");
    ASSERT_EQ(b.size(), 3u);
    EXPECT_EQ(b[0].quoteDepth, 1);
    EXPECT_EQ(b[1].quoteDepth, 2);
    EXPECT_EQ(b[2].quoteDepth, 0);
}

TEST(Markdown, Tables) {
    auto b = parse("| a | b |\n|---|---|\n| **1** | 2 |\n| 3 | 4 |");
    ASSERT_EQ(b.size(), 1u);
    ASSERT_EQ(b[0].kind, Kind::Table);
    ASSERT_EQ(b[0].rows.size(), 3u);
    EXPECT_EQ(plain(b[0].rows[0][1]), "b");
    EXPECT_TRUE(b[0].rows[1][0][0].bold);
    EXPECT_EQ(plain(b[0].rows[2][1]), "4");
}

TEST(Markdown, LinksAndAutolinks) {
    auto b = parse("veja [o site](https://x.com/a?b=1&amp;c=2) ou https://y.com/p e www.z.com.");
    ASSERT_EQ(b.size(), 1u);
    std::vector<std::pair<std::string, std::string>> links;
    for (const auto& s : b[0].spans)
        if (!s.link.empty()) links.push_back({s.text, s.link});
    ASSERT_EQ(links.size(), 3u);
    EXPECT_EQ(links[0], (std::pair<std::string, std::string>{"o site", "https://x.com/a?b=1&c=2"}));
    EXPECT_EQ(links[1].second, "https://y.com/p");
    EXPECT_EQ(links[2].second, "http://www.z.com");
}

TEST(Markdown, RawHtmlIsText) {
    auto b = parse("<script>alert(1)</script> &lt;b&gt; &#233;");
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(plain(b[0].spans), "<script>alert(1)</script> <b> é");
}

TEST(Markdown, ImagesBecomeTheirOwnBlocks) {
    auto b = parse("antes\n![print.png](https://trello.com/1/cards/c/attachments/a/download/print.png \"t\")\ndepois");
    ASSERT_EQ(b.size(), 3u);
    EXPECT_EQ(plain(b[0].spans), "antes");
    EXPECT_EQ(b[1].kind, Kind::Image);
    EXPECT_EQ(b[1].url, "https://trello.com/1/cards/c/attachments/a/download/print.png");
    EXPECT_EQ(b[1].text, "print.png");
    EXPECT_EQ(plain(b[2].spans), "depois");
}

TEST(Markdown, ABareLinkToAPictureOnItsOwnLineIsShownAsThePicture) {
    auto b = parse("olha:\n\nhttps://x.com/erro.jpg\n\nhttps://x.com/page e https://x.com/b.png no meio");
    ASSERT_EQ(b.size(), 3u);
    EXPECT_EQ(b[1].kind, Kind::Image);
    EXPECT_EQ(b[1].url, "https://x.com/erro.jpg");
    EXPECT_EQ(b[2].kind, Kind::Paragraph);  // inside a sentence it stays a link
}

TEST(Markdown, ImagesInsideLinksAndTablesStayLinks) {
    auto b = parse("[![logo](https://x.com/l.png)](https://x.com)\n\n| a |\n|---|\n| ![i](https://x.com/i.png) |");
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0].kind, Kind::Paragraph);
    EXPECT_EQ(b[0].spans[0].link, "https://x.com");
    EXPECT_EQ(b[0].spans[0].text, "logo");
    EXPECT_EQ(b[1].rows[1][0][0].link, "https://x.com/i.png");
}

TEST(Markdown, CardShortLinkOf) {
    EXPECT_EQ(cardShortLinkOf("https://trello.com/c/AbC123xy/12-um-titulo"), "AbC123xy");
    EXPECT_EQ(cardShortLinkOf("https://trello.com/c/AbC123xy"), "AbC123xy");
    EXPECT_EQ(cardShortLinkOf("http://www.trello.com/c/AbC123xy?x=1"), "AbC123xy");
    EXPECT_EQ(cardShortLinkOf("HTTPS://Trello.com/c/AbC123xy#a"), "AbC123xy");
    EXPECT_EQ(cardShortLinkOf("https://trello.com/b/AbC123xy/quadro"), "");
    EXPECT_EQ(cardShortLinkOf("https://trello.com/c/"), "");
    EXPECT_EQ(cardShortLinkOf("https://trello.com.evil.com/c/AbC123xy"), "");
    EXPECT_EQ(cardShortLinkOf("https://nottrello.com/c/AbC123xy"), "");
    EXPECT_EQ(cardShortLinkOf("https://trello.com/c/Ab-C"), "");
}

TEST(Markdown, LinksToCardsShowTheCardNameUnlessTheyHaveTheirOwnText) {
    auto b = parse("ver https://trello.com/c/AbC123xy/1-x e "
                   "[https://trello.com/c/Zz9/2](https://trello.com/c/Zz9/2 \"smartCard-inline\") e "
                   "[este card](https://trello.com/c/Qq1)");
    ASSERT_EQ(b.size(), 1u);
    std::vector<Span> cards;
    for (const auto& s : b[0].spans)
        if (!s.cardShortLink.empty()) cards.push_back(s);
    ASSERT_EQ(cards.size(), 3u);
    EXPECT_EQ(cards[0].cardShortLink, "AbC123xy");
    EXPECT_TRUE(cards[0].cardName);
    EXPECT_EQ(cards[1].cardShortLink, "Zz9");
    EXPECT_TRUE(cards[1].cardName);
    EXPECT_EQ(cards[2].cardShortLink, "Qq1");
    EXPECT_FALSE(cards[2].cardName);
    EXPECT_EQ(cards[2].text, "este card");
}

TEST(Markdown, CardShortLinksIn) {
    EXPECT_TRUE(cardShortLinksIn("nenhum link").empty());
    EXPECT_EQ(cardShortLinksIn("https://trello.com/c/A1 e [b](https://trello.com/c/B2) e https://trello.com/c/A1\n\n"
                               "| x |\n|---|\n| https://trello.com/c/C3 |\n\n`https://trello.com/c/D4`"),
              (std::vector<std::string>{"A1", "B2", "C3"}));
}
