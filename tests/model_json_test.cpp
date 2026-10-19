#include <gtest/gtest.h>

#include <fstream>
#include <nlohmann/json.hpp>

#include "cardinal/model.h"

namespace {

nlohmann::json fixture(const std::string& name) {
    std::ifstream in(std::string(FIXTURE_DIR) + "/" + name);
    return nlohmann::json::parse(in);
}

}  // namespace

using namespace cardinal;

TEST(ParseCard, ReadsCoreFields) {
    Card c = parseCard(fixture("card_full.json"));
    EXPECT_EQ(c.id, "c1");
    EXPECT_EQ(c.listId, "l1");
    EXPECT_EQ(c.name, "Corrigir login SSO");
    EXPECT_EQ(c.desc, "Detalhes\nem duas linhas");
    EXPECT_DOUBLE_EQ(c.pos, 16384.5);
    EXPECT_EQ(c.labelIds, (std::vector<std::string>{"lb1", "lb2"}));
    EXPECT_EQ(c.memberIds, (std::vector<std::string>{"m1"}));
    EXPECT_EQ(c.commentCount, 3);
    EXPECT_EQ(c.dateLastActivity, "2026-10-19T12:00:00.000Z");
}

TEST(ParseCard, MissingOptionalFieldsAreEmpty) {
    Card c = parseCard(fixture("card_minimal.json"));
    EXPECT_EQ(c.id, "c2");
    EXPECT_EQ(c.desc, "");
    EXPECT_DOUBLE_EQ(c.pos, 0.0);
    EXPECT_TRUE(c.labelIds.empty());
    EXPECT_TRUE(c.memberIds.empty());
    EXPECT_EQ(c.commentCount, 0);
    EXPECT_EQ(c.dateLastActivity, "");
}

TEST(ParseCard, Utf8Emoji) {
    Card c = parseCard(fixture("card_emoji.json"));
    EXPECT_EQ(c.name, "Café ☕ 🚀");
    EXPECT_EQ(c.desc, "日本語 ñ");
}

TEST(ParseBoard, ReadsFields) {
    Board b = parseBoard({{"id", "b1"}, {"name", "Produto"}, {"closed", true}});
    EXPECT_EQ(b.id, "b1");
    EXPECT_EQ(b.name, "Produto");
    EXPECT_TRUE(b.closed);
}

TEST(ParseList, ReadsFieldsAndDefaultsCardCount) {
    List l = parseList({{"id", "l1"}, {"idBoard", "b1"}, {"name", "A fazer"}, {"pos", 2.0}});
    EXPECT_EQ(l.id, "l1");
    EXPECT_EQ(l.boardId, "b1");
    EXPECT_EQ(l.name, "A fazer");
    EXPECT_DOUBLE_EQ(l.pos, 2.0);
    EXPECT_EQ(l.cardCount, 0);
}

TEST(ParseLabel, NullNameAndColorBecomeEmpty) {
    Label l = parseLabel({{"id", "lb1"}, {"name", nullptr}, {"color", nullptr}});
    EXPECT_EQ(l.id, "lb1");
    EXPECT_EQ(l.name, "");
    EXPECT_EQ(l.color, "");
}

TEST(ParseMember, ReadsFields) {
    Member m = parseMember({{"id", "m1"}, {"fullName", "Vitor M"}, {"initials", "VM"}});
    EXPECT_EQ(m.fullName, "Vitor M");
    EXPECT_EQ(m.initials, "VM");
}

TEST(ParseComment, ReadsCommentActionShape) {
    Comment c = parseComment({{"id", "a1"},
                              {"idMemberCreator", "m1"},
                              {"date", "2026-10-19T12:00:00.000Z"},
                              {"data", {{"text", "olá 👋"}}}});
    EXPECT_EQ(c.id, "a1");
    EXPECT_EQ(c.authorId, "m1");
    EXPECT_EQ(c.text, "olá 👋");
    EXPECT_EQ(c.date, "2026-10-19T12:00:00.000Z");
}
