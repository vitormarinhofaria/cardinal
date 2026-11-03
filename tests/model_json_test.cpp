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

TEST(ParseCard, ReadsClosedFlagAndDefaultsToOpen) {
    EXPECT_TRUE(parseCard({{"id", "c"}, {"closed", true}}).closed);
    EXPECT_FALSE(parseCard({{"id", "c"}}).closed);
}

TEST(ModelJson, AttachmentPicksTheSmallestPreviewThatIsStillSharp) {
    auto j = nlohmann::json::parse(R"({"id":"a","name":"f.png","url":"https://trello.com/f.png",
        "mimeType":"image/png","isUpload":true,"previews":[
          {"url":"https://trello.com/p150","width":150,"height":100},
          {"url":"https://trello.com/p600","width":600,"height":400},
          {"url":"https://trello.com/p1200","width":1200,"height":800},
          {"url":"https://trello.com/p500","width":500,"height":333}]})");
    Attachment a = parseAttachment(j);
    EXPECT_EQ(a.mimeType, "image/png");
    EXPECT_TRUE(a.isUpload);
    EXPECT_EQ(a.previewUrl, "https://trello.com/p500");
    EXPECT_EQ(a.previewWidth, 500);
    EXPECT_EQ(a.previewHeight, 333);
}

TEST(ModelJson, AttachmentWithOnlySmallPreviewsTakesTheBiggest) {
    auto j = nlohmann::json::parse(R"({"id":"a","previews":[
          {"url":"https://trello.com/p70","width":70,"height":50},
          {"url":"https://trello.com/p150","width":150,"height":100}]})");
    EXPECT_EQ(parseAttachment(j).previewUrl, "https://trello.com/p150");
    EXPECT_TRUE(parseAttachment(nlohmann::json::parse(R"({"id":"a"})")).previewUrl.empty());
}

TEST(ParseBoard, PictureBackgroundPicksAScaledVersionWideEnoughForAScreen) {
    Board b = parseBoard(nlohmann::json::parse(R"({"id":"b1","name":"X","prefs":{
        "backgroundColor":"#8a7a6b","backgroundTopColor":"#9a8a7b","backgroundBottomColor":"#5a4a3b",
        "backgroundBrightness":"dark","backgroundImage":"https://bg.example/original.jpg",
        "backgroundImageScaled":[{"width":480,"height":320,"url":"https://bg.example/480.jpg"},
                                 {"width":2560,"height":1707,"url":"https://bg.example/2560.jpg"},
                                 {"width":1920,"height":1280,"url":"https://bg.example/1920.jpg"},
                                 {"width":3200,"height":2133,"url":"https://bg.example/3200.jpg"}]}})"));
    EXPECT_EQ(b.background.imageUrl, "https://bg.example/1920.jpg");
    EXPECT_EQ(b.background.color, "#8a7a6b");
    EXPECT_EQ(b.background.topColor, "#9a8a7b");
    EXPECT_EQ(b.background.bottomColor, "#5a4a3b");
    EXPECT_EQ(b.background.brightness, "dark");
}

TEST(ParseBoard, PictureBackgroundFallsBackToLargestScaledThenOriginal) {
    Board small = parseBoard(nlohmann::json::parse(R"({"prefs":{"backgroundImage":"https://bg.example/o.jpg",
        "backgroundImageScaled":[{"width":480,"url":"https://bg.example/480.jpg"},
                                 {"width":1024,"url":"https://bg.example/1024.jpg"}]}})"));
    EXPECT_EQ(small.background.imageUrl, "https://bg.example/1024.jpg");
    Board original = parseBoard(nlohmann::json::parse(
        R"({"prefs":{"backgroundImage":"https://bg.example/o.jpg","backgroundImageScaled":null}})"));
    EXPECT_EQ(original.background.imageUrl, "https://bg.example/o.jpg");
}

TEST(ParseBoard, GradientBackgroundUsesItsColorsNotTheSvg) {
    Board b = parseBoard(nlohmann::json::parse(R"({"prefs":{"background":"gradient-snow",
        "backgroundImage":"https://trello.example/gradients/snow.svg",
        "backgroundTopColor":"#0c66e4","backgroundBottomColor":"#37b4c3","backgroundBrightness":"dark"}})"));
    EXPECT_TRUE(b.background.imageUrl.empty());
    EXPECT_EQ(b.background.topColor, "#0c66e4");
    EXPECT_EQ(b.background.bottomColor, "#37b4c3");
}

TEST(ParseBoard, NoPrefsMeansNoBackground) {
    Board b = parseBoard(nlohmann::json::parse(R"({"id":"b1","name":"X","prefs":null})"));
    EXPECT_EQ(b.background, BoardBackground{});
}

TEST(ParseList, ReadsColorAndNullIsEmpty) {
    EXPECT_EQ(parseList(nlohmann::json::parse(R"({"id":"l1","color":"teal"})")).color, "teal");
    EXPECT_EQ(parseList(nlohmann::json::parse(R"({"id":"l1","color":null})")).color, "");
}
