#include <gtest/gtest.h>

#include <sqlite3.h>

#include <filesystem>
#include <thread>

#include "cardinal/store.h"

using namespace cardinal;

namespace {

class StoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("cardinal-store-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        path_ = dir_ / "cache.db";
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    static Card makeCard(std::string id, std::string listId, std::string name, double pos = 1) {
        Card c;
        c.id = std::move(id);
        c.listId = std::move(listId);
        c.name = std::move(name);
        c.pos = pos;
        return c;
    }

    std::filesystem::path dir_, path_;
};

}  // namespace

TEST_F(StoreTest, RoundTripsCard) {
    Store s = Store::open(path_);
    Card c = makeCard("c1", "l1", "Café ☕ 🚀");
    c.desc = "linha1\nlinha2";
    c.labelIds = {"lb1", "lb2"};
    c.memberIds = {"m1"};
    c.commentCount = 4;
    c.dateLastActivity = "2026-10-19T12:00:00.000Z";
    std::vector<Card> in{c};
    s.upsertCards(in);

    auto out = s.card("c1");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->listId, "l1");
    EXPECT_EQ(out->name, "Café ☕ 🚀");
    EXPECT_EQ(out->desc, "linha1\nlinha2");
    EXPECT_EQ(out->labelIds, (std::vector<std::string>{"lb1", "lb2"}));
    EXPECT_EQ(out->memberIds, (std::vector<std::string>{"m1"}));
    EXPECT_EQ(out->commentCount, 4);
    EXPECT_EQ(out->dateLastActivity, "2026-10-19T12:00:00.000Z");
}

TEST_F(StoreTest, UpsertUpdatesExisting) {
    Store s = Store::open(path_);
    std::vector<Card> first{makeCard("c1", "l1", "antigo")};
    s.upsertCards(first);
    std::vector<Card> second{makeCard("c1", "l2", "novo")};
    s.upsertCards(second);

    EXPECT_EQ(s.card("c1")->name, "novo");
    EXPECT_EQ(s.card("c1")->listId, "l2");
    EXPECT_TRUE(s.cards("l1").empty());
    EXPECT_EQ(s.cards("l2").size(), 1u);
}

TEST_F(StoreTest, ReopenKeepsData) {
    {
        Store s = Store::open(path_);
        std::vector<Board> b{{"b1", "Produto", false}};
        std::vector<List> l{{"l1", "b1", "A fazer", 1, 0}};
        std::vector<Card> c{makeCard("c1", "l1", "x")};
        s.upsertBoards(b);
        s.upsertLists(l);
        s.upsertCards(c);
        s.setSyncCursor("b1", "2026-10-19T12:00:00.000Z");
    }
    Store s = Store::open(path_);
    ASSERT_EQ(s.boards().size(), 1u);
    EXPECT_EQ(s.boards()[0].name, "Produto");
    ASSERT_EQ(s.lists("b1").size(), 1u);
    EXPECT_EQ(s.lists("b1")[0].name, "A fazer");
    EXPECT_EQ(s.cards("l1").size(), 1u);
    EXPECT_EQ(s.syncCursor("b1"), "2026-10-19T12:00:00.000Z");
}

TEST_F(StoreTest, EmptyListReturnsEmpty) {
    Store s = Store::open(path_);
    EXPECT_TRUE(s.cards("nada").empty());
    EXPECT_TRUE(s.lists("nada").empty());
    EXPECT_FALSE(s.card("nada").has_value());
    EXPECT_FALSE(s.syncCursor("nada").has_value());
}

TEST_F(StoreTest, CardsAndListsComeOrderedByPos) {
    Store s = Store::open(path_);
    std::vector<Card> cards{makeCard("a", "l1", "a", 30), makeCard("b", "l1", "b", 10),
                            makeCard("c", "l1", "c", 20)};
    s.upsertCards(cards);
    auto out = s.cards("l1");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].id, "b");
    EXPECT_EQ(out[1].id, "c");
    EXPECT_EQ(out[2].id, "a");
}

TEST_F(StoreTest, UpsertListKeepsCardCountOfCachedCards) {
    Store s = Store::open(path_);
    std::vector<List> l{{"l1", "b1", "A fazer", 1, 24}};
    s.upsertLists(l);
    EXPECT_EQ(s.lists("b1")[0].cardCount, 24);
}

TEST_F(StoreTest, SyncCursorOverwrites) {
    Store s = Store::open(path_);
    s.setSyncCursor("b1", "t1");
    s.setSyncCursor("b1", "t2");
    EXPECT_EQ(s.syncCursor("b1"), "t2");
}

TEST_F(StoreTest, TouchListIsReadBack) {
    Store s = Store::open(path_);
    EXPECT_FALSE(s.listTouchedAt("l1").has_value());
    s.touchList("l1", 1000);
    s.touchList("l1", 5000000000LL);
    EXPECT_EQ(s.listTouchedAt("l1"), std::time_t{5000000000LL});
}

TEST_F(StoreTest, SummaryUpsertKeepsCachedDescription) {
    Store s = Store::open(path_);
    Card full = makeCard("c1", "l1", "velho");
    full.desc = "descrição detalhada";
    std::vector<Card> a{full};
    s.upsertCards(a);

    Card summary = makeCard("c1", "l2", "novo", 5);  // list fetches carry no desc
    std::vector<Card> b{summary, makeCard("c2", "l2", "outro")};
    s.upsertCardSummaries(b);

    EXPECT_EQ(s.card("c1")->desc, "descrição detalhada");
    EXPECT_EQ(s.card("c1")->name, "novo");
    EXPECT_EQ(s.card("c1")->listId, "l2");
    EXPECT_EQ(s.card("c2")->desc, "");
}

TEST_F(StoreTest, PruneListCardsRemovesOnlyMissingCardsOfThatList) {
    Store s = Store::open(path_);
    std::vector<Card> cards{makeCard("a", "l1", "a"), makeCard("b", "l1", "b"),
                            makeCard("c", "l2", "c")};
    s.upsertCards(cards);
    std::vector<Comment> cm{{"x1", "m1", "oi", "d"}};
    s.replaceComments("b", cm);

    auto removed = s.pruneListCards("l1", std::vector<std::string>{"a"});
    EXPECT_EQ(removed, (std::vector<std::string>{"b"}));
    EXPECT_TRUE(s.card("a").has_value());
    EXPECT_FALSE(s.card("b").has_value());
    EXPECT_TRUE(s.card("c").has_value());
    EXPECT_TRUE(s.comments("b").empty());
}

TEST_F(StoreTest, PruneListCardsWithEmptyKeepRemovesAll) {
    Store s = Store::open(path_);
    std::vector<Card> cards{makeCard("a", "l1", "a")};
    s.upsertCards(cards);
    EXPECT_EQ(s.pruneListCards("l1", {}).size(), 1u);
    EXPECT_TRUE(s.cards("l1").empty());
}

TEST_F(StoreTest, PruneBoardListsRemovesListsAndTheirCards) {
    Store s = Store::open(path_);
    std::vector<List> lists{{"l1", "b1", "um", 1, 0}, {"l2", "b1", "dois", 2, 0},
                            {"l9", "b2", "outro board", 1, 0}};
    s.upsertLists(lists);
    std::vector<Card> cards{makeCard("a", "l1", "a"), makeCard("b", "l2", "b")};
    s.upsertCards(cards);

    s.pruneBoardLists("b1", std::vector<std::string>{"l1"});
    EXPECT_EQ(s.lists("b1").size(), 1u);
    EXPECT_EQ(s.lists("b1")[0].id, "l1");
    EXPECT_EQ(s.lists("b2").size(), 1u);
    EXPECT_FALSE(s.card("b").has_value());
    EXPECT_TRUE(s.card("a").has_value());
}

TEST_F(StoreTest, PruneBoardsRemovesBoardsNotKept) {
    Store s = Store::open(path_);
    std::vector<Board> b{{"b1", "um", false}, {"b2", "dois", false}};
    s.upsertBoards(b);
    s.pruneBoards(std::vector<std::string>{"b2"});
    ASSERT_EQ(s.boards().size(), 1u);
    EXPECT_EQ(s.boards()[0].id, "b2");
}

TEST_F(StoreTest, LabelsAndMembersAreReplacedPerBoard) {
    Store s = Store::open(path_);
    s.replaceLabels("b1", std::vector<Label>{{"lb1", "Bug", "red"}, {"lb2", "", ""}});
    s.replaceMembers("b1", std::vector<Member>{{"m1", "Vitor M", "VM"}});
    s.replaceLabels("b2", std::vector<Label>{{"lb9", "Outro", "blue"}});
    ASSERT_EQ(s.labels("b1").size(), 2u);
    EXPECT_EQ(s.labels("b1")[0].name, "Bug");
    s.replaceLabels("b1", std::vector<Label>{{"lb1", "Bug2", "red"}});
    ASSERT_EQ(s.labels("b1").size(), 1u);
    EXPECT_EQ(s.labels("b1")[0].name, "Bug2");
    EXPECT_EQ(s.labels("b2").size(), 1u);
    ASSERT_EQ(s.members("b1").size(), 1u);
    EXPECT_EQ(s.members("b1")[0].initials, "VM");
    EXPECT_TRUE(s.members("nada").empty());
}

TEST_F(StoreTest, CommentsKeepOrderAndAreReplacedPerCard) {
    Store s = Store::open(path_);
    s.replaceComments("c1", std::vector<Comment>{{"a2", "m1", "segundo 👋", "d2"},
                                                 {"a1", "m2", "primeiro", "d1"}});
    auto out = s.comments("c1");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].id, "a2");
    EXPECT_EQ(out[0].text, "segundo 👋");
    EXPECT_EQ(out[1].authorId, "m2");
    s.replaceComments("c1", {});
    EXPECT_TRUE(s.comments("c1").empty());
}

TEST_F(StoreTest, DeleteCardRemovesCardAndComments) {
    Store s = Store::open(path_);
    std::vector<Card> cards{makeCard("a", "l1", "a")};
    s.upsertCards(cards);
    s.replaceComments("a", std::vector<Comment>{{"x", "m", "t", "d"}});
    s.deleteCard("a");
    EXPECT_FALSE(s.card("a").has_value());
    EXPECT_TRUE(s.comments("a").empty());
    s.deleteCard("a");  // deleting twice is harmless
}

TEST_F(StoreTest, MigratesV1DatabaseKeepingData) {
    {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(path_.string().c_str(), &db), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db, R"sql(
            CREATE TABLE boards(id TEXT PRIMARY KEY, name TEXT NOT NULL, closed INTEGER NOT NULL);
            CREATE TABLE lists(id TEXT PRIMARY KEY, board_id TEXT NOT NULL, name TEXT NOT NULL,
                pos REAL NOT NULL, card_count INTEGER NOT NULL);
            CREATE TABLE cards(id TEXT PRIMARY KEY, list_id TEXT NOT NULL, name TEXT NOT NULL,
                desc TEXT NOT NULL, pos REAL NOT NULL, label_ids TEXT NOT NULL,
                member_ids TEXT NOT NULL, comment_count INTEGER NOT NULL,
                date_last_activity TEXT NOT NULL);
            CREATE TABLE sync_cursor(board_id TEXT PRIMARY KEY, cursor TEXT NOT NULL);
            CREATE TABLE list_touch(list_id TEXT PRIMARY KEY, touched_at INTEGER NOT NULL);
            CREATE TABLE pending_ops(id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL,
                card_id TEXT NOT NULL, payload TEXT NOT NULL, created_at INTEGER NOT NULL);
            INSERT INTO boards VALUES('b1','Antigo',0);
            PRAGMA user_version = 1;
        )sql", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
    }
    Store s = Store::open(path_);
    ASSERT_EQ(s.boards().size(), 1u);
    EXPECT_EQ(s.boards()[0].name, "Antigo");
    s.replaceComments("c1", std::vector<Comment>{{"a", "m", "t", "d"}});  // v2 table exists
    EXPECT_EQ(s.comments("c1").size(), 1u);
}

TEST_F(StoreTest, ConcurrentWritersAndReadersDoNotCorruptData) {
    Store s = Store::open(path_);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&s, t] {
            for (int i = 0; i < 50; ++i) {
                std::vector<Card> c{makeCard("c" + std::to_string(t) + "_" + std::to_string(i),
                                             "l" + std::to_string(t), "x")};
                s.upsertCards(c);
                (void)s.cards("l" + std::to_string(t));
            }
        });
    }
    for (auto& th : threads) th.join();
    for (int t = 0; t < 4; ++t) EXPECT_EQ(s.cards("l" + std::to_string(t)).size(), 50u);
}
