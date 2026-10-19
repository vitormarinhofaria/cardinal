#include <gtest/gtest.h>

#include <filesystem>

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
