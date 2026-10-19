#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <thread>

#include "cardinal/sync.h"
#include "fakes.h"

using namespace cardinal;
using namespace cardinal::testing;

namespace {

class QueueExecutor : public Executor {
public:
    void post(std::function<void()> job) override { jobs.push_back(std::move(job)); }
    bool runOne() {
        if (jobs.empty()) return false;
        auto job = std::move(jobs.front());
        jobs.pop_front();
        job();
        return true;
    }
    void drain() {
        while (runOne()) {}
    }
    std::deque<std::function<void()>> jobs;
};

struct RecordingSink : ChangeSink {
    void onChanged(const ChangeSet& c) override {
        std::lock_guard lk(mu);
        changes.push_back(c);
    }
    void onError(const SyncError& e) override {
        std::lock_guard lk(mu);
        errors.push_back(e);
    }
    bool any(std::vector<std::string> ChangeSet::*field, const std::string& id) const {
        std::lock_guard lk(mu);
        for (const auto& c : changes)
            if (std::count((c.*field).begin(), (c.*field).end(), id)) return true;
        return false;
    }
    mutable std::mutex mu;
    std::vector<ChangeSet> changes;
    std::vector<SyncError> errors;
};

constexpr const char* kBoards = R"([{"id":"b1","name":"Produto","closed":false}])";
constexpr const char* kBoardInfo = R"({"id":"b1","name":"Produto","closed":false,
    "labels":[{"id":"lb1","name":"Bug","color":"red"}],
    "members":[{"id":"m1","fullName":"Vitor M","initials":"VM"}]})";
constexpr const char* kLists = R"([
    {"id":"l1","name":"A fazer","pos":1,"cards":[{"id":"c1"},{"id":"c2"}]},
    {"id":"l2","name":"Fazendo","pos":2,"cards":[{"id":"c3"}]},
    {"id":"l3","name":"Arquivo","pos":3,"cards":[{"id":"c4"},{"id":"c5"},{"id":"c6"}]}])";
constexpr const char* kL1Cards = R"([
    {"id":"c1","idList":"l1","name":"Corrigir login","pos":1,"idLabels":["lb1"],"idMembers":["m1"],
     "badges":{"comments":1},"dateLastActivity":"2026-10-19T10:00:00.000Z"},
    {"id":"c2","idList":"l1","name":"Revisar","pos":2}])";
constexpr const char* kL2Cards = R"([{"id":"c3","idList":"l2","name":"Migrar","pos":1}])";
constexpr const char* kL3Cards = R"([{"id":"c4","idList":"l3","name":"a","pos":1},
    {"id":"c5","idList":"l3","name":"b","pos":2},{"id":"c6","idList":"l3","name":"c","pos":3}])";
constexpr const char* kCardCore = R"({"id":"c1","idList":"l1","name":"Corrigir login","desc":"novo",
    "pos":1,"idLabels":["lb1"],"idMembers":["m1"],"badges":{"comments":1},
    "dateLastActivity":"2026-10-19T11:00:00.000Z",
    "actions":[{"id":"a1","idMemberCreator":"m1","date":"d","data":{"text":"primeiro"}}]})";
constexpr const char* kCardExtras = R"({"id":"c1",
    "checklists":[{"id":"ck1","name":"Passos","checkItems":[{"id":"i1","name":"um","state":"complete"}]}],
    "attachments":[{"id":"at1","name":"spec.pdf","url":"https://x/spec.pdf"}],
    "members":[{"id":"m1","fullName":"Vitor M","initials":"VM"}]})";

class SyncTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("cardinal-sync-" + std::string(::testing::UnitTest::GetInstance()
                                                   ->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        store = std::make_unique<Store>(Store::open(dir_ / "cache.db"));
        req.routes = {{"/members/me/boards", "", {200, kBoards}},
                      {"/boards/b1/lists", "", {200, kLists}},
                      {"/boards/b1", "", {200, kBoardInfo}},
                      {"/lists/l1/cards", "", {200, kL1Cards}},
                      {"/lists/l2/cards", "", {200, kL2Cards}},
                      {"/lists/l3/cards", "", {200, kL3Cards}},
                      {"/cards/c1", "checklists=all", {200, kCardExtras}},
                      {"/cards/c1", "", {200, kCardCore}}};
        opts.alwaysHotLists = 1;
        opts.smallListThreshold = 0;
        opts.prefetchCold = false;
        opts.now = [this] { return now; };
    }
    void TearDown() override {
        engine.reset();
        store.reset();
        std::filesystem::remove_all(dir_);
    }
    void makeEngine() {
        engine = std::make_unique<SyncEngine>(*api, *store, exec, sink, opts);
    }
    static Card card(std::string id, std::string listId, std::string name, std::string desc = "") {
        Card c;
        c.id = std::move(id);
        c.listId = std::move(listId);
        c.name = std::move(name);
        c.desc = std::move(desc);
        return c;
    }
    std::vector<Submitted> callsTo(const std::string& path) {
        std::vector<Submitted> out;
        for (const auto& c : req.calls)
            if (c.url.find("https://api.trello.com/1" + path + "?") == 0) out.push_back(c);
        return out;
    }

    std::filesystem::path dir_;
    std::unique_ptr<Store> store;
    RouteRequester req;
    TrelloApi apiObj{req, "K", "T"};
    TrelloApi* api = &apiObj;
    QueueExecutor exec;
    RecordingSink sink;
    SyncOptions opts;
    std::time_t now = 1'000'000;
    std::unique_ptr<SyncEngine> engine;
};

}  // namespace

TEST_F(SyncTest, StartCachesBoardsAndPrunesRemovedOnes) {
    std::vector<Board> stale{{"gone", "Removido", false}};
    store->upsertBoards(stale);
    makeEngine();
    engine->start();
    exec.drain();
    ASSERT_EQ(engine->boards().size(), 1u);
    EXPECT_EQ(engine->boards()[0].id, "b1");
    EXPECT_TRUE(sink.changes.back().boards);
    EXPECT_TRUE(sink.errors.empty());
}

TEST_F(SyncTest, FirstRunLoadsListsAndHotCardsOnly) {
    makeEngine();
    engine->openBoard("b1");
    exec.drain();

    ASSERT_EQ(engine->lists("b1").size(), 3u);
    EXPECT_EQ(engine->lists("b1")[2].cardCount, 3);
    EXPECT_EQ(engine->cards("l1").size(), 2u);
    EXPECT_TRUE(engine->cards("l2").empty());
    EXPECT_TRUE(engine->cards("l3").empty());
    EXPECT_EQ(engine->labels("b1").size(), 1u);
    EXPECT_EQ(engine->members("b1").size(), 1u);
    EXPECT_TRUE(sink.any(&ChangeSet::boardLists, "b1"));
    EXPECT_TRUE(sink.any(&ChangeSet::listCards, "l1"));
    EXPECT_FALSE(sink.any(&ChangeSet::listCards, "l2"));
    EXPECT_EQ(callsTo("/lists/l1/cards").size(), 1u);
    EXPECT_EQ(callsTo("/lists/l1/cards")[0].priority, Priority::Visible);
    EXPECT_TRUE(callsTo("/lists/l2/cards").empty());
}

TEST_F(SyncTest, RecentlyTouchedListIsHotOnNextOpen) {
    store->touchList("l2", now - 3600);   // an hour ago
    store->touchList("l3", now - 60LL * 24 * 3600 * 30);  // a month ago: cold
    makeEngine();
    engine->openBoard("b1");
    exec.drain();
    EXPECT_EQ(callsTo("/lists/l2/cards").size(), 1u);
    EXPECT_TRUE(callsTo("/lists/l3/cards").empty());
}

TEST_F(SyncTest, SmallListsAreLoadedEagerly) {
    opts.smallListThreshold = 1;  // l2 has 1 card
    makeEngine();
    engine->openBoard("b1");
    exec.drain();
    EXPECT_EQ(callsTo("/lists/l2/cards").size(), 1u);
    EXPECT_TRUE(callsTo("/lists/l3/cards").empty());
}

TEST_F(SyncTest, ColdListLoadsOnExpandAtInteractivePriority) {
    makeEngine();
    engine->openBoard("b1");
    exec.drain();
    engine->expandList("l3");
    exec.drain();

    EXPECT_EQ(engine->cards("l3").size(), 3u);
    ASSERT_EQ(callsTo("/lists/l3/cards").size(), 1u);
    EXPECT_EQ(callsTo("/lists/l3/cards")[0].priority, Priority::Interactive);
    EXPECT_EQ(store->listTouchedAt("l3"), now);
    EXPECT_TRUE(sink.any(&ChangeSet::listCards, "l3"));
}

TEST_F(SyncTest, ColdListsPrefetchedAtBackgroundPriorityInOrder) {
    opts.prefetchCold = true;
    makeEngine();
    engine->openBoard("b1");
    exec.drain();

    std::vector<std::string> order;
    for (const auto& c : req.calls)
        if (c.url.find("/lists/") != std::string::npos)
            order.push_back(c.url.substr(c.url.find("/lists/") + 7, 2) + ":" +
                            std::to_string(static_cast<int>(c.priority)));
    EXPECT_EQ(order, (std::vector<std::string>{"l1:1", "l2:2", "l3:2"}));
    EXPECT_EQ(engine->cards("l3").size(), 3u);
}

TEST_F(SyncTest, ColdPrefetchRunsOneListAtATime) {
    opts.prefetchCold = true;
    makeEngine();
    engine->openBoard("b1");
    exec.runOne();  // snapshot job
    exec.runOne();  // hot list l1
    ASSERT_EQ(exec.jobs.size(), 1u);  // only the head of the cold chain is queued
    exec.runOne();  // loads l2, queues l3
    EXPECT_EQ(exec.jobs.size(), 1u);
}

TEST_F(SyncTest, SwitchingBoardStopsColdPrefetchOfPreviousBoard) {
    opts.prefetchCold = true;
    req.routes.insert(req.routes.begin(),
                      {{"/boards/b2/lists", "", {200, R"([{"id":"l9","name":"x","pos":1,"cards":[]}])"}},
                       {"/boards/b2", "", {200, R"({"id":"b2","name":"Outro","labels":[],"members":[]})"}},
                       {"/lists/l9/cards", "", {200, "[]"}}});
    makeEngine();
    engine->openBoard("b1");
    exec.runOne();
    exec.runOne();  // b1 hot list done; cold chain for b1 queued
    engine->openBoard("b2");
    exec.drain();
    EXPECT_TRUE(callsTo("/lists/l2/cards").empty());
    EXPECT_TRUE(callsTo("/lists/l3/cards").empty());
    EXPECT_EQ(callsTo("/boards/b2/lists").size(), 1u);
}

TEST_F(SyncTest, ListRefreshKeepsCachedDescription) {
    std::vector<Card> cached{card("c1", "l1", "velho", "descrição já baixada")};
    store->upsertCards(cached);
    makeEngine();
    engine->openBoard("b1");
    exec.drain();
    EXPECT_EQ(engine->card("c1")->name, "Corrigir login");
    EXPECT_EQ(engine->card("c1")->desc, "descrição já baixada");
}

TEST_F(SyncTest, CardsGoneFromServerListAreRemovedFromCache) {
    std::vector<Card> cached{card("c1", "l1", "x"), card("c9", "l1", "arquivado")};
    store->upsertCards(cached);
    std::vector<List> staleLists{{"l7", "b1", "lista apagada", 9, 1}};
    store->upsertLists(staleLists);
    std::vector<Card> inStale{card("c8", "l7", "preso")};
    store->upsertCards(inStale);
    makeEngine();
    engine->openBoard("b1");
    exec.drain();
    EXPECT_FALSE(engine->card("c9").has_value());
    EXPECT_FALSE(engine->card("c8").has_value());
    for (const auto& l : engine->lists("b1")) EXPECT_NE(l.id, "l7");
}

TEST_F(SyncTest, OpenCardServesCacheThenRefreshes) {
    std::vector<Card> cached{card("c1", "l1", "Corrigir login", "antiga")};
    store->upsertCards(cached);
    makeEngine();
    EXPECT_EQ(engine->card("c1")->desc, "antiga");  // instant, before any network

    engine->openCard("c1");
    EXPECT_TRUE(req.calls.empty());  // nothing runs on the caller's thread
    exec.drain();

    EXPECT_EQ(engine->card("c1")->desc, "novo");
    ASSERT_EQ(engine->comments("c1").size(), 1u);
    EXPECT_EQ(engine->comments("c1")[0].text, "primeiro");
    EXPECT_TRUE(sink.any(&ChangeSet::cardDetails, "c1"));
    auto core = callsTo("/cards/c1");
    ASSERT_EQ(core.size(), 2u);
    EXPECT_EQ(core[0].priority, Priority::Interactive);
    EXPECT_EQ(store->listTouchedAt("l1"), now);
}

TEST_F(SyncTest, OpenCardLoadsExtrasAfterCore) {
    makeEngine();
    engine->openCard("c1");
    exec.runOne();  // core
    EXPECT_TRUE(sink.any(&ChangeSet::cardDetails, "c1"));
    EXPECT_FALSE(sink.any(&ChangeSet::cardExtras, "c1"));
    EXPECT_FALSE(engine->extras("c1").has_value());

    exec.runOne();  // extras
    EXPECT_TRUE(sink.any(&ChangeSet::cardExtras, "c1"));
    auto extras = engine->extras("c1");
    ASSERT_TRUE(extras.has_value());
    EXPECT_EQ(extras->checklists.size(), 1u);
    EXPECT_EQ(extras->attachments[0].name, "spec.pdf");
    auto calls = callsTo("/cards/c1");
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[1].priority, Priority::Visible);
}

TEST_F(SyncTest, OpenCardMovedRemotelyNotifiesBothLists) {
    std::vector<Card> cached{card("c1", "l2", "x")};  // cache thinks l2, server says l1
    store->upsertCards(cached);
    makeEngine();
    engine->openCard("c1");
    exec.drain();
    EXPECT_EQ(engine->card("c1")->listId, "l1");
    EXPECT_TRUE(sink.any(&ChangeSet::listCards, "l1"));
    EXPECT_TRUE(sink.any(&ChangeSet::listCards, "l2"));
}

TEST_F(SyncTest, OpenCardDeletedRemotelyIsRemovedAndReported) {
    req.routes.insert(req.routes.begin(), {"/cards/c1", "", {404, "not found"}});
    std::vector<Card> cached{card("c1", "l1", "x")};
    store->upsertCards(cached);
    makeEngine();
    engine->openCard("c1");
    exec.drain();
    EXPECT_FALSE(engine->card("c1").has_value());
    EXPECT_TRUE(sink.any(&ChangeSet::removedCards, "c1"));
    EXPECT_TRUE(sink.any(&ChangeSet::listCards, "l1"));
    EXPECT_TRUE(sink.errors.empty());  // a vanished card is a change, not an error
}

TEST_F(SyncTest, OpenCardArchivedRemotelyIsRemovedAndReported) {
    req.routes.insert(req.routes.begin(),
                      {"/cards/c1", "", {200, R"({"id":"c1","idList":"l1","name":"x","closed":true})"}});
    std::vector<Card> cached{card("c1", "l1", "x")};
    store->upsertCards(cached);
    makeEngine();
    engine->openCard("c1");
    exec.drain();
    EXPECT_FALSE(engine->card("c1").has_value());
    EXPECT_TRUE(sink.any(&ChangeSet::removedCards, "c1"));
    EXPECT_FALSE(engine->extras("c1").has_value());
}

TEST_F(SyncTest, UnauthorizedReportsStatusAndKeepsCache) {
    std::vector<Board> cached{{"b1", "Em cache", false}};
    store->upsertBoards(cached);
    req.routes = {{"/members/me/boards", "", {401, "invalid token"}}};
    makeEngine();
    engine->start();
    exec.drain();
    ASSERT_EQ(sink.errors.size(), 1u);
    EXPECT_EQ(sink.errors[0].status, 401);
    ASSERT_EQ(engine->boards().size(), 1u);
    EXPECT_EQ(engine->boards()[0].name, "Em cache");
}

TEST_F(SyncTest, OfflineReportsTransportErrorAndKeepsServingCache) {
    std::vector<Card> cached{card("c1", "l1", "x", "em cache")};
    store->upsertCards(cached);
    req.failAll = true;
    makeEngine();
    engine->openCard("c1");
    exec.drain();
    ASSERT_FALSE(sink.errors.empty());
    EXPECT_EQ(sink.errors[0].status, 0);
    EXPECT_EQ(engine->card("c1")->desc, "em cache");
}

TEST_F(SyncTest, JobsQueuedBeforeDestructionBecomeNoOps) {
    makeEngine();
    engine->openBoard("b1");
    engine->openCard("c1");
    engine.reset();  // engine gone, jobs still queued
    exec.drain();    // must not touch the dead engine or send requests
    EXPECT_TRUE(req.calls.empty());
}

TEST_F(SyncTest, RealThreadPoolLoadsEverythingWithoutRaces) {
    ThreadPoolExecutor pool(4);
    opts.prefetchCold = true;
    {
        SyncEngine real(*api, *store, pool, sink, opts);
        real.start();
        real.openBoard("b1");
        for (int i = 0; i < 20; ++i) real.openCard("c1");
        real.expandList("l3");

        for (int i = 0; i < 500 && (real.cards("l3").size() < 3 || real.cards("l2").empty() ||
                                    !real.extras("c1").has_value() || real.boards().empty());
             ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        EXPECT_EQ(real.boards().size(), 1u);
        EXPECT_EQ(real.cards("l1").size(), 2u);
        EXPECT_EQ(real.cards("l2").size(), 1u);
        EXPECT_EQ(real.cards("l3").size(), 3u);
        EXPECT_EQ(real.card("c1")->desc, "novo");
        EXPECT_TRUE(real.extras("c1").has_value());
    }  // engine destroyed while the pool is still alive: must not hang or crash
}
