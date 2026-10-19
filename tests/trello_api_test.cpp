#include <gtest/gtest.h>

#include <map>
#include <sstream>

#include "cardinal/trello_api.h"
#include "fakes.h"

using namespace cardinal;
using namespace cardinal::testing;

namespace {

std::string decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// "a=b&c=d" -> {a:b, c:d}, percent-decoded
std::map<std::string, std::string> parseQuery(const std::string& q) {
    std::map<std::string, std::string> out;
    std::stringstream ss(q);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        auto eq = pair.find('=');
        out[decode(pair.substr(0, eq))] = eq == std::string::npos ? "" : decode(pair.substr(eq + 1));
    }
    return out;
}

std::string pathOf(const std::string& url) {
    auto base = std::string("https://api.trello.com/1");
    EXPECT_EQ(url.rfind(base, 0), 0u) << url;
    auto rest = url.substr(base.size());
    return rest.substr(0, rest.find('?'));
}

std::map<std::string, std::string> queryOf(const std::string& url) {
    auto q = url.find('?');
    return q == std::string::npos ? std::map<std::string, std::string>{}
                                  : parseQuery(url.substr(q + 1));
}

class ApiTest : public ::testing::Test {
protected:
    FakeRequester req;
    TrelloApi api{req, "K", "T"};
};

}  // namespace

TEST(UrlEncode, EncodesUtf8AndReservedBytes) {
    EXPECT_EQ(urlEncode("Café ☕ 🚀"), "Caf%C3%A9%20%E2%98%95%20%F0%9F%9A%80");
    EXPECT_EQ(urlEncode("a&b=c?d"), "a%26b%3Dc%3Fd");
    EXPECT_EQ(urlEncode("safe-._~09AZaz"), "safe-._~09AZaz");
    EXPECT_EQ(urlEncode(""), "");
}

TEST_F(ApiTest, BoardsAsksOnlyNameAndClosedForOpenBoards) {
    req.script = {{200, R"([{"id":"b1","name":"Produto","closed":false}])"}};
    auto boards = api.boards().get();
    ASSERT_EQ(boards.size(), 1u);
    EXPECT_EQ(boards[0].name, "Produto");
    ASSERT_EQ(req.calls.size(), 1u);
    EXPECT_EQ(req.calls[0].method, Method::Get);
    EXPECT_EQ(pathOf(req.calls[0].url), "/members/me/boards");
    auto q = queryOf(req.calls[0].url);
    EXPECT_EQ(q["fields"], "name,closed");
    EXPECT_EQ(q["filter"], "open");
    EXPECT_EQ(q["key"], "K");
    EXPECT_EQ(q["token"], "T");
}

TEST_F(ApiTest, BoardSnapshotCombinesInfoAndListCountsWithoutDescOrComments) {
    req.script = {
        {200, R"({"id":"b1","name":"Produto","closed":false,
                  "labels":[{"id":"lb1","name":"Bug","color":"red"}],
                  "members":[{"id":"m1","fullName":"Vitor M","initials":"VM"}]})"},
        {200, R"([{"id":"l1","name":"A fazer","pos":1,"cards":[{"id":"c1"},{"id":"c2"}]},
                  {"id":"l2","name":"Feito","pos":2,"cards":[]}])"}};
    auto snap = api.board("b1", Priority::Visible).get();

    EXPECT_EQ(snap.board.id, "b1");
    EXPECT_EQ(snap.board.name, "Produto");
    ASSERT_EQ(snap.lists.size(), 2u);
    EXPECT_EQ(snap.lists[0].id, "l1");
    EXPECT_EQ(snap.lists[0].boardId, "b1");
    EXPECT_EQ(snap.lists[0].cardCount, 2);
    EXPECT_EQ(snap.lists[1].cardCount, 0);
    ASSERT_EQ(snap.labels.size(), 1u);
    EXPECT_EQ(snap.labels[0].color, "red");
    ASSERT_EQ(snap.members.size(), 1u);
    EXPECT_EQ(snap.members[0].initials, "VM");

    ASSERT_EQ(req.calls.size(), 2u);
    for (const auto& c : req.calls) {
        EXPECT_EQ(c.priority, Priority::Visible);
        EXPECT_EQ(c.url.find("desc"), std::string::npos) << c.url;
        EXPECT_EQ(c.url.find("actions"), std::string::npos) << c.url;
        EXPECT_EQ(c.url.find("comments"), std::string::npos) << c.url;
    }
    auto info = queryOf(req.calls[0].url);
    EXPECT_EQ(pathOf(req.calls[0].url), "/boards/b1");
    EXPECT_EQ(info["labels"], "all");
    EXPECT_EQ(info["label_limit"], "1000");
    EXPECT_EQ(info["members"], "all");
    auto lists = queryOf(req.calls[1].url);
    EXPECT_EQ(pathOf(req.calls[1].url), "/boards/b1/lists");
    EXPECT_EQ(lists["filter"], "open");
    EXPECT_EQ(lists["cards"], "open");
    EXPECT_EQ(lists["card_fields"], "id");
}

TEST_F(ApiTest, EmptyBoardIsAValidSnapshot) {
    req.script = {{200, R"({"id":"b1","name":"Vazio","closed":false,"labels":[],"members":[]})"},
                  {200, "[]"}};
    auto snap = api.board("b1", Priority::Interactive).get();
    EXPECT_TRUE(snap.lists.empty());
    EXPECT_TRUE(snap.labels.empty());
}

TEST_F(ApiTest, ListCardsAsksMinimalFieldsAtGivenPriority) {
    req.script = {{200, R"([{"id":"c1","idList":"l1","name":"x","pos":1,"badges":{"comments":2}}])"}};
    auto cards = api.listCards("l1", Priority::Background).get();
    ASSERT_EQ(cards.size(), 1u);
    EXPECT_EQ(cards[0].commentCount, 2);
    ASSERT_EQ(req.calls.size(), 1u);
    EXPECT_EQ(req.calls[0].priority, Priority::Background);
    EXPECT_EQ(pathOf(req.calls[0].url), "/lists/l1/cards");
    auto q = queryOf(req.calls[0].url);
    EXPECT_EQ(q["fields"], "name,idList,pos,idLabels,idMembers,dateLastActivity,badges");
}

TEST_F(ApiTest, CardCoreIsInteractiveAndParsesDescriptionAndComments) {
    req.script = {{200, R"({"id":"c1","idList":"l1","name":"N","desc":"D 🚀","pos":1,
        "idLabels":["lb1"],"idMembers":[],"badges":{"comments":1},"dateLastActivity":"t",
        "actions":[{"id":"a1","idMemberCreator":"m1","date":"d","data":{"text":"oi"}}]})"}};
    auto core = api.cardCore("c1").get();
    EXPECT_EQ(core.card.desc, "D 🚀");
    EXPECT_EQ(core.card.labelIds, (std::vector<std::string>{"lb1"}));
    ASSERT_EQ(core.comments.size(), 1u);
    EXPECT_EQ(core.comments[0].text, "oi");
    EXPECT_EQ(core.comments[0].authorId, "m1");

    ASSERT_EQ(req.calls.size(), 1u);
    EXPECT_EQ(req.calls[0].priority, Priority::Interactive);
    EXPECT_EQ(pathOf(req.calls[0].url), "/cards/c1");
    auto q = queryOf(req.calls[0].url);
    EXPECT_EQ(q["actions"], "commentCard");
    EXPECT_NE(q["fields"].find("desc"), std::string::npos);
    EXPECT_NE(q["fields"].find("closed"), std::string::npos);  // archived cards must be detectable
    EXPECT_EQ(q["checklists"], "");  // heavy data stays out of the core request
    EXPECT_EQ(q["attachments"], "");
}

TEST_F(ApiTest, CardCoreWithoutCommentsHasEmptyList) {
    req.script = {{200, R"({"id":"c1","idList":"l1","name":"N"})"}};
    EXPECT_TRUE(api.cardCore("c1").get().comments.empty());
}

TEST_F(ApiTest, CardExtrasParsesChecklistsAttachmentsMembersAtVisiblePriority) {
    req.script = {{200, R"({"id":"c1",
        "checklists":[{"id":"ck1","name":"Passos","checkItems":[
            {"id":"i1","name":"um","state":"complete"},{"id":"i2","name":"dois","state":"incomplete"}]}],
        "attachments":[{"id":"at1","name":"spec.pdf","url":"https://x/spec.pdf"}],
        "members":[{"id":"m1","fullName":"Vitor M","initials":"VM"}]})"}};
    auto extras = api.cardExtras("c1").get();
    ASSERT_EQ(extras.checklists.size(), 1u);
    EXPECT_EQ(extras.checklists[0].name, "Passos");
    ASSERT_EQ(extras.checklists[0].items.size(), 2u);
    EXPECT_TRUE(extras.checklists[0].items[0].done);
    EXPECT_FALSE(extras.checklists[0].items[1].done);
    ASSERT_EQ(extras.attachments.size(), 1u);
    EXPECT_EQ(extras.attachments[0].url, "https://x/spec.pdf");
    ASSERT_EQ(extras.members.size(), 1u);
    EXPECT_EQ(req.calls[0].priority, Priority::Visible);
    auto q = queryOf(req.calls[0].url);
    EXPECT_EQ(q["checklists"], "all");
    EXPECT_EQ(q["attachments"], "true");
    EXPECT_EQ(q["members"], "true");
}

TEST_F(ApiTest, ActionsSinceUsesCursorAtBackgroundPriority) {
    req.script = {{200, R"([
        {"id":"a2","type":"commentCard","date":"2026-10-19T12:05:00.000Z","idMemberCreator":"m1",
         "data":{"text":"oi","card":{"id":"c1"}}},
        {"id":"a1","type":"updateCard","date":"2026-10-19T12:01:00.000Z","idMemberCreator":"m2",
         "data":{"card":{"id":"c1"},"listBefore":{"id":"l1"},"listAfter":{"id":"l2"}}}])"}};
    auto actions = api.actionsSince("b1", "2026-10-19T12:00:00.000Z").get();
    ASSERT_EQ(actions.size(), 2u);
    EXPECT_EQ(actions[0].type, "commentCard");
    EXPECT_EQ(actions[0].cardId, "c1");
    EXPECT_EQ(actions[0].memberId, "m1");
    EXPECT_EQ(actions[1].listId, "l2");
    EXPECT_NE(actions[0].dataJson.find("oi"), std::string::npos);

    EXPECT_EQ(req.calls[0].priority, Priority::Background);
    EXPECT_EQ(pathOf(req.calls[0].url), "/boards/b1/actions");
    auto q = queryOf(req.calls[0].url);
    EXPECT_EQ(q["since"], "2026-10-19T12:00:00.000Z");
    EXPECT_EQ(q["limit"], "1000");
    EXPECT_NE(q["filter"].find("commentCard"), std::string::npos);
    EXPECT_NE(q["filter"].find("updateCard"), std::string::npos);
}

TEST_F(ApiTest, MoveCardPutsListAndPosInBody) {
    req.script = {{200, R"({"id":"c1","idList":"l2","name":"N","pos":512.5})"}};
    Card c = api.moveCard("c1", "l2", 512.5).get();
    EXPECT_EQ(c.listId, "l2");
    ASSERT_EQ(req.calls.size(), 1u);
    EXPECT_EQ(req.calls[0].priority, Priority::Interactive);
    EXPECT_EQ(req.calls[0].method, Method::Put);
    EXPECT_EQ(pathOf(req.calls[0].url), "/cards/c1");
    auto body = parseQuery(req.calls[0].body);
    EXPECT_EQ(body["idList"], "l2");
    EXPECT_EQ(std::stod(body["pos"]), 512.5);
    auto q = queryOf(req.calls[0].url);
    EXPECT_EQ(q["key"], "K");
    EXPECT_EQ(q["token"], "T");
    EXPECT_EQ(q.count("idList"), 0u);
}

TEST_F(ApiTest, CreateCardPostsToBottomOfList) {
    req.script = {{200, R"({"id":"c9","idList":"l1","name":"Novo"})"}};
    Card c = api.createCard("l1", "Novo").get();
    EXPECT_EQ(c.id, "c9");
    EXPECT_EQ(req.calls[0].method, Method::Post);
    EXPECT_EQ(pathOf(req.calls[0].url), "/cards");
    auto body = parseQuery(req.calls[0].body);
    EXPECT_EQ(body["idList"], "l1");
    EXPECT_EQ(body["name"], "Novo");
    EXPECT_EQ(body["pos"], "bottom");
    EXPECT_EQ(req.calls[0].priority, Priority::Interactive);
}

TEST_F(ApiTest, UpdateCardEncodesUtf8AndLongTextInBody) {
    req.script = {{200, R"({"id":"c1","idList":"l1","name":"x"})"}};
    std::string longDesc(50000, 'x');
    longDesc += " café ☕ & = ?";
    api.updateCard("c1", {std::string("Café ☕ 🚀"), longDesc}).get();
    EXPECT_EQ(req.calls[0].method, Method::Put);
    EXPECT_LT(req.calls[0].url.size(), 200u);  // nothing bulky in the URL
    auto body = parseQuery(req.calls[0].body);
    EXPECT_EQ(body["name"], "Café ☕ 🚀");
    EXPECT_EQ(body["desc"], longDesc);
}

TEST_F(ApiTest, UpdateCardSendsOnlyTheGivenFields) {
    req.script = {{200, R"({"id":"c1","idList":"l1","name":"x"})"}};
    api.updateCard("c1", {std::nullopt, std::string("só descrição")}).get();
    auto body = parseQuery(req.calls[0].body);
    EXPECT_EQ(body.count("name"), 0u);
    EXPECT_EQ(body["desc"], "só descrição");
}

TEST_F(ApiTest, UpdateCardCanClearDescription) {
    req.script = {{200, R"({"id":"c1","idList":"l1","name":"x"})"}};
    api.updateCard("c1", {std::nullopt, std::string("")}).get();
    auto body = parseQuery(req.calls[0].body);
    EXPECT_EQ(body.count("desc"), 1u);
    EXPECT_EQ(body["desc"], "");
}

TEST_F(ApiTest, AddLabelAndAddMemberPostValue) {
    api.addLabel("c1", "lb1").get();
    api.addMember("c1", "m1").get();
    ASSERT_EQ(req.calls.size(), 2u);
    EXPECT_EQ(req.calls[0].method, Method::Post);
    EXPECT_EQ(pathOf(req.calls[0].url), "/cards/c1/idLabels");
    EXPECT_EQ(parseQuery(req.calls[0].body)["value"], "lb1");
    EXPECT_EQ(pathOf(req.calls[1].url), "/cards/c1/idMembers");
    EXPECT_EQ(parseQuery(req.calls[1].body)["value"], "m1");
    EXPECT_EQ(req.calls[0].priority, Priority::Interactive);
    EXPECT_EQ(req.calls[1].priority, Priority::Interactive);
}

TEST_F(ApiTest, AddCommentPostsTextAndReturnsComment) {
    req.script = {{200, R"({"id":"a7","idMemberCreator":"m1","date":"d","data":{"text":"olá 👋"}})"}};
    Comment c = api.addComment("c1", "olá 👋").get();
    EXPECT_EQ(c.id, "a7");
    EXPECT_EQ(c.text, "olá 👋");
    EXPECT_EQ(req.calls[0].method, Method::Post);
    EXPECT_EQ(pathOf(req.calls[0].url), "/cards/c1/actions/comments");
    EXPECT_EQ(parseQuery(req.calls[0].body)["text"], "olá 👋");
}

TEST_F(ApiTest, HttpErrorStatusBecomesApiError) {
    req.script = {{401, "invalid token"}};
    try {
        api.boards().get();
        FAIL() << "expected ApiError";
    } catch (const ApiError& e) {
        EXPECT_EQ(e.status(), 401);
        EXPECT_EQ(e.body(), "invalid token");
    }
}

TEST_F(ApiTest, NotFoundOnCardCoreIsApiError404) {
    req.script = {{404, "The requested resource was not found."}};
    try {
        api.cardCore("gone").get();
        FAIL() << "expected ApiError";
    } catch (const ApiError& e) {
        EXPECT_EQ(e.status(), 404);
    }
}

TEST_F(ApiTest, InvalidJsonBecomesRuntimeError) {
    req.script = {{200, "<html>oops</html>"}};
    EXPECT_THROW(api.boards().get(), std::runtime_error);
}

TEST_F(ApiTest, TransportFailureOfAnyBoardRequestPropagates) {
    req.script = {{200, R"({"id":"b1","name":"x","labels":[],"members":[]})"}, {500, "boom"}};
    EXPECT_THROW(api.board("b1", Priority::Visible).get(), ApiError);
}

TEST_F(ApiTest, CardFieldsAsksOnlyWhatConflictCheckNeedsAtInteractivePriority) {
    req.script = {{200, R"({"id":"c1","name":"N","desc":"D","dateLastActivity":"2026-10-19T12:00:00.000Z"})"}};
    CardFields f = api.cardFields("c1").get();
    EXPECT_EQ(f.name, "N");
    EXPECT_EQ(f.desc, "D");
    EXPECT_EQ(f.dateLastActivity, "2026-10-19T12:00:00.000Z");
    ASSERT_EQ(req.calls.size(), 1u);
    EXPECT_EQ(req.calls[0].method, Method::Get);
    EXPECT_EQ(req.calls[0].priority, Priority::Interactive);
    EXPECT_EQ(pathOf(req.calls[0].url), "/cards/c1");
    EXPECT_EQ(queryOf(req.calls[0].url)["fields"], "name,desc,dateLastActivity");
}
