#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <thread>

#include "cardinal/viewmodel.h"
#include "fakes.h"

using namespace cardinal;
using namespace cardinal::testing;

namespace {

struct FakeUi : UiDispatcher {
    void post(std::function<void()> fn) override { queue.push_back(std::move(fn)); }
    bool runAll() {
        bool any = false;
        while (!queue.empty()) {
            auto fn = std::move(queue.front());
            queue.pop_front();
            inDispatch = true;
            fn();
            inDispatch = false;
            any = true;
        }
        return any;
    }
    std::deque<std::function<void()>> queue;
    bool inDispatch = false;
};

struct FakeEvents : EventSource {
    void watch(const std::string& b, Handler h) override { handlers[b] = std::move(h); }
    void unwatch(const std::string& b) override { handlers.erase(b); }
    std::map<std::string, Handler> handlers;
};

constexpr const char* kBoards = R"([{"id":"b1","name":"Produto","closed":false}])";
constexpr const char* kBoardInfo = R"({"id":"b1","name":"Produto","closed":false,
    "labels":[{"id":"lb1","name":"Bug","color":"red"},{"id":"lb2","name":"Feature","color":"green"}],
    "members":[{"id":"m1","fullName":"Vitor M","initials":"VM"},{"id":"m2","fullName":"Ana B","initials":"AB"}]})";
constexpr const char* kLists = R"([
    {"id":"l1","name":"A fazer","pos":1,"cards":[{"id":"c1"},{"id":"c2"}]},
    {"id":"l2","name":"Fazendo","pos":2,"cards":[{"id":"c3"}]},
    {"id":"l3","name":"Arquivo","pos":3,"cards":[{"id":"c4"},{"id":"c5"},{"id":"c6"}]}])";
constexpr const char* kL1Cards = R"([
    {"id":"c1","idList":"l1","name":"Corrigir login","pos":1,"idLabels":["lb1"],"idMembers":["m1"],
     "badges":{"comments":2},"dateLastActivity":"t0"},
    {"id":"c2","idList":"l1","name":"Revisar","pos":2}])";
constexpr const char* kL2Cards = R"([{"id":"c3","idList":"l2","name":"Migrar","pos":4}])";
constexpr const char* kL3Cards = R"([{"id":"c4","idList":"l3","name":"a","pos":1},
    {"id":"c5","idList":"l3","name":"b","pos":2},{"id":"c6","idList":"l3","name":"c","pos":3}])";
constexpr const char* kCardCore = R"({"id":"c1","idList":"l1","name":"Corrigir login","desc":"novo",
    "pos":1,"idLabels":["lb1"],"idMembers":["m1"],"badges":{"comments":1},"dateLastActivity":"t1",
    "actions":[{"id":"a1","idMemberCreator":"m2","date":"2026-10-19T12:00:00.000Z","data":{"text":"primeiro"}}]})";
constexpr const char* kCardExtras = R"({"id":"c1",
    "checklists":[{"id":"ck1","name":"Passos","checkItems":[{"id":"i1","name":"um","state":"complete"}]}],
    "attachments":[{"id":"at1","name":"spec.pdf","url":"https://x/spec.pdf"}],"members":[]})";

class VmTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("cardinal-vm-" + std::string(::testing::UnitTest::GetInstance()
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
                      {"/cards/c1", "fields=name%2Cdesc%2CdateLastActivity",
                       {200, R"({"id":"c1","name":"Corrigir login","desc":"novo","dateLastActivity":"t1"})"}},
                      {"/cards/c1", "checklists=all", {200, kCardExtras}},
                      {"/cards/c1", "actions=commentCard", {200, kCardCore}}};
        sync.alwaysHotLists = 1;
        sync.smallListThreshold = 0;
        sync.prefetchCold = false;
    }
    void TearDown() override {
        vmsAndApp.clear();
        app.reset();
        store.reset();
        std::filesystem::remove_all(dir_);
    }
    void make() { app = std::make_unique<AppModel>(api, *store, exec, ui, &events, sync, write); }
    void pump() {
        for (int i = 0; i < 10000; ++i) {
            bool progress = false;
            while (exec.runOne()) progress = true;
            if (ui.runAll()) progress = true;
            if (!progress) return;
        }
        FAIL() << "pump did not settle";
    }
    void routeFirst(Method m, const std::string& path, int status, const std::string& body,
                    const std::string& contains = "") {
        req.routes.insert(req.routes.begin(), {path, contains, {status, body}, m});
    }
    std::vector<Submitted> sent(Method m, const std::string& path) {
        std::vector<Submitted> out;
        for (const auto& c : req.calls)
            if (c.method == m && c.url.find("https://api.trello.com/1" + path + "?") == 0)
                out.push_back(c);
        return out;
    }
    static ColumnState col(const std::shared_ptr<const ColumnState>& c) { return *c; }
    static Card card(std::string id, std::string listId, std::string name, double pos,
                     std::string desc = "") {
        Card c;
        c.id = id;
        c.listId = listId;
        c.name = name;
        c.pos = pos;
        c.desc = desc;
        return c;
    }

    std::filesystem::path dir_;
    std::unique_ptr<Store> store;
    RouteRequester req;
    TrelloApi api{req, "K", "T"};
    QueueExecutor exec;
    FakeUi ui;
    FakeEvents events;
    SyncOptions sync;
    WriteOptions write;
    std::unique_ptr<AppModel> app;
    std::vector<std::shared_ptr<void>> vmsAndApp;
};

}  // namespace

TEST_F(VmTest, NotifiesOnUiDispatcherOnly) {
    make();
    auto board = app->openBoard("b1");
    int structure = 0, columns = 0;
    bool offThread = false;
    board->structureChanged.connect([&] {
        ++structure;
        offThread |= !ui.inDispatch;
    });
    board->columnChanged.connect([&](const std::string&) {
        ++columns;
        offThread |= !ui.inDispatch;
    });
    auto picker = app->boardPicker();
    int pickerEvents = 0;
    picker->state().changed.connect([&](const BoardPickerState&) {
        ++pickerEvents;
        offThread |= !ui.inDispatch;
    });
    app->start();

    while (exec.runOne()) {}  // all background work, no UI turn yet
    EXPECT_EQ(structure + columns + pickerEvents, 0);

    ui.runAll();
    pump();
    EXPECT_GT(structure, 0);
    EXPECT_GT(columns, 0);
    EXPECT_GT(pickerEvents, 0);
    EXPECT_FALSE(offThread);
}

TEST_F(VmTest, BoardPickerShowsCacheThenRefreshes) {
    std::vector<Board> cached{{"b0", "Em cache", false}};
    store->upsertBoards(cached);
    make();
    app->start();
    exec.runOne();  // first background job: cache read
    ui.runAll();
    ASSERT_EQ(app->boardPicker()->state().get().boards.size(), 1u);
    EXPECT_EQ(app->boardPicker()->state().get().boards[0].name, "Em cache");
    EXPECT_TRUE(req.calls.empty());

    pump();
    ASSERT_EQ(app->boardPicker()->state().get().boards.size(), 1u);
    EXPECT_EQ(app->boardPicker()->state().get().boards[0].name, "Produto");
    EXPECT_FALSE(app->boardPicker()->state().get().loading);
}

TEST_F(VmTest, BoardShowsColumnsWithResolvedChipsAndColdListsUnloaded) {
    make();
    auto board = app->openBoard("b1");
    pump();

    EXPECT_EQ(board->header().get().name, "Produto");
    EXPECT_FALSE(board->header().get().loading);
    auto cols = board->columns();
    ASSERT_EQ(cols.size(), 3u);
    EXPECT_EQ(cols[0]->name, "A fazer");
    EXPECT_TRUE(cols[0]->loaded);
    ASSERT_EQ(cols[0]->cards.size(), 2u);
    const CardRow& row = cols[0]->cards[0];
    EXPECT_EQ(row.title, "Corrigir login");
    ASSERT_EQ(row.labels.size(), 1u);
    EXPECT_EQ(row.labels[0].name, "Bug");
    EXPECT_EQ(row.labels[0].color, "red");
    ASSERT_EQ(row.members.size(), 1u);
    EXPECT_EQ(row.members[0].initials, "VM");
    EXPECT_EQ(row.commentCount, 2);
    EXPECT_FALSE(cols[1]->loaded);
    EXPECT_EQ(cols[1]->cardCount, 1);
    EXPECT_TRUE(cols[1]->cards.empty());
    EXPECT_FALSE(cols[2]->loaded);
    EXPECT_EQ(cols[2]->cardCount, 3);
    EXPECT_EQ(board->labels().size(), 2u);
    EXPECT_EQ(board->members().size(), 2u);
}

TEST_F(VmTest, ExpandListLoadsItAndPublishesOnlyThatColumn) {
    make();
    auto board = app->openBoard("b1");
    pump();
    std::vector<std::string> changed;
    board->columnChanged.connect([&](const std::string& id) { changed.push_back(id); });
    int structure = 0;
    board->structureChanged.connect([&] { ++structure; });

    board->expandList("l3");
    pump();
    EXPECT_EQ(changed, (std::vector<std::string>{"l3"}));
    EXPECT_EQ(structure, 0);
    EXPECT_TRUE(board->column("l3")->loaded);
    EXPECT_EQ(board->column("l3")->cards.size(), 3u);
}

TEST_F(VmTest, EmptyBoardIsValid) {
    req.routes = {{"/boards/b1/lists", "", {200, "[]"}},
                  {"/boards/b1", "", {200, R"({"id":"b1","name":"Vazio","labels":[],"members":[]})"}},
                  {"/members/me/boards", "", {200, R"([{"id":"b1","name":"Vazio","closed":false}])"}}};
    make();
    app->start();
    auto board = app->openBoard("b1");
    pump();
    EXPECT_TRUE(board->columns().empty());
    EXPECT_TRUE(board->labels().empty());
    EXPECT_EQ(board->header().get().name, "Vazio");
    EXPECT_FALSE(board->header().get().loading);
    EXPECT_EQ(board->column("nada"), nullptr);
}

TEST_F(VmTest, MoveCardUsesPositionBetweenNeighboursAndUpdatesBothColumns) {
    make();
    auto board = app->openBoard("b1");
    board->expandList("l2");
    pump();
    routeFirst(Method::Put, "/cards/c3", 200,
               R"({"id":"c3","idList":"l1","name":"Migrar","pos":1.5,"dateLastActivity":"t9"})");
    std::vector<std::string> changed;
    board->columnChanged.connect([&](const std::string& id) { changed.push_back(id); });

    board->moveCard("c3", "l1", 1);  // between c1 (pos 1) and c2 (pos 2)
    pump();

    auto puts = sent(Method::Put, "/cards/c3");
    ASSERT_EQ(puts.size(), 1u);
    auto form = parseForm(puts[0].body);
    EXPECT_EQ(form["idList"], "l1");
    EXPECT_DOUBLE_EQ(std::stod(form["pos"]), 1.5);
    auto l1 = board->column("l1");
    ASSERT_EQ(l1->cards.size(), 3u);
    EXPECT_EQ(l1->cards[1].id, "c3");
    EXPECT_TRUE(board->column("l2")->cards.empty());
    EXPECT_NE(std::find(changed.begin(), changed.end(), "l1"), changed.end());
    EXPECT_NE(std::find(changed.begin(), changed.end(), "l2"), changed.end());
}

TEST_F(VmTest, MoveCardToTopBottomAndWithinSameColumn) {
    make();
    auto board = app->openBoard("b1");
    board->expandList("l2");
    pump();
    routeFirst(Method::Put, "/cards/c3", 200, R"({"id":"c3","idList":"l1","name":"Migrar","pos":0.5})");
    board->moveCard("c3", "l1", 0);
    pump();
    EXPECT_DOUBLE_EQ(std::stod(parseForm(sent(Method::Put, "/cards/c3").back().body)["pos"]), 0.5);

    board->moveCard("c3", "l1", 2);  // others = c1, c2 -> after the last one
    pump();
    EXPECT_DOUBLE_EQ(std::stod(parseForm(sent(Method::Put, "/cards/c3").back().body)["pos"]),
                     2 + 65536);

    routeFirst(Method::Put, "/cards/c1", 200, R"({"id":"c1","idList":"l1","name":"x","pos":3})");
    board->moveCard("c1", "l1", 1);  // within the column: c1 leaves, so slot 1 is after c2
    pump();
    double pos = std::stod(parseForm(sent(Method::Put, "/cards/c1").back().body)["pos"]);
    EXPECT_GT(pos, 2);
}

TEST_F(VmTest, CreateCardShowsTemporaryCardThenTheRealOne) {
    make();
    auto board = app->openBoard("b1");
    pump();
    routeFirst(Method::Post, "/cards", 200,
               R"({"id":"c9","idList":"l1","name":"Novo","pos":65536,"dateLastActivity":"t3"})");
    board->createCard("l1", "Novo");
    exec.runOne();  // the enqueue job: optimistic card is in the cache now
    exec.runOne();  // coalesced rebuild
    ui.runAll();
    auto cards = board->column("l1")->cards;
    ASSERT_EQ(cards.size(), 3u);
    EXPECT_EQ(cards[2].title, "Novo");
    EXPECT_EQ(cards[2].id.rfind("tmp-", 0), 0u);

    pump();
    cards = board->column("l1")->cards;
    ASSERT_EQ(cards.size(), 3u);
    EXPECT_EQ(cards[2].id, "c9");
}

TEST_F(VmTest, CardPanelShowsCacheImmediatelyBeforeAnyNetwork) {
    std::vector<Card> cached{card("c1", "l1", "Corrigir login", 1, "descrição em cache")};
    cached[0].labelIds = {"lb1"};
    store->upsertCards(cached);
    std::vector<List> lists{{"l1", "b1", "A fazer", 1, 2}};
    store->upsertLists(lists);
    store->replaceLabels("b1", std::vector<Label>{{"lb1", "Bug", "red"}});
    store->replaceComments("c1", std::vector<Comment>{{"a0", "m1", "comentário antigo", "d"}});
    make();
    auto board = app->openBoard("b1");  // tells the app which board the card belongs to
    pump();
    req.calls.clear();

    auto panel = app->openCard("c1");
    exec.runOne();  // only the cache job
    ui.runAll();
    const auto& s = panel->state().get();
    EXPECT_EQ(s.status, CardPanelState::Status::Ready);
    EXPECT_EQ(s.title, "Corrigir login");
    EXPECT_EQ(s.description, "descrição em cache");
    EXPECT_EQ(s.listName, "A fazer");
    ASSERT_EQ(s.labels.size(), 1u);
    EXPECT_EQ(s.labels[0].name, "Bug");
    ASSERT_EQ(s.comments.size(), 1u);
    EXPECT_FALSE(s.extrasLoaded);
    EXPECT_TRUE(req.calls.empty());
}

TEST_F(VmTest, CardPanelUpdatesWhenDetailsAndExtrasArrive) {
    make();
    auto board = app->openBoard("b1");
    pump();
    auto panel = app->openCard("c1");
    pump();
    const auto& s = panel->state().get();
    EXPECT_EQ(s.status, CardPanelState::Status::Ready);
    EXPECT_EQ(s.description, "novo");
    ASSERT_EQ(s.comments.size(), 1u);
    EXPECT_EQ(s.comments[0].text, "primeiro");
    EXPECT_EQ(s.comments[0].authorName, "Ana B");
    EXPECT_EQ(s.comments[0].authorInitials, "AB");
    EXPECT_TRUE(s.extrasLoaded);
    ASSERT_EQ(s.checklists.size(), 1u);
    EXPECT_EQ(s.attachments[0].name, "spec.pdf");
    EXPECT_EQ(s.boardLabels.size(), 2u);
    EXPECT_EQ(s.boardMembers.size(), 2u);
}

TEST_F(VmTest, PanelWarnsWhenCardRemovedRemotely) {
    routeFirst(Method::Get, "/cards/c1", 404, "not found");
    make();
    auto board = app->openBoard("b1");
    pump();
    auto panel = app->openCard("c1");
    pump();
    const auto& s = panel->state().get();
    EXPECT_EQ(s.status, CardPanelState::Status::Removed);
    ASSERT_EQ(s.notices.size(), 1u);
    EXPECT_EQ(s.notices[0].kind, PanelNotice::Kind::RemovedRemotely);
}

TEST_F(VmTest, PanelEditsGoThroughTheWriteQueueAndShowAtOnce) {
    routeFirst(Method::Put, "/cards/c1", 200, R"({"id":"c1","idList":"l1","name":"Novo título","pos":1})");
    routeFirst(Method::Post, "/cards/c1/idLabels", 200, "[]");
    routeFirst(Method::Post, "/cards/c1/idMembers", 200, "[]");
    routeFirst(Method::Post, "/cards/c1/actions/comments", 200,
               R"({"id":"a9","idMemberCreator":"m1","date":"d","data":{"text":"olá"}})");
    make();
    auto board = app->openBoard("b1");
    pump();
    auto panel = app->openCard("c1");
    pump();

    panel->setTitle("Novo título");
    panel->setDescription("nova descrição");
    panel->addLabel("lb2");
    panel->addMember("m2");
    panel->addComment("olá");
    for (int i = 0; i < 5; ++i) exec.runOne();  // the five enqueue jobs, in order
    exec.runOne();                               // coalesced rebuild of the board column
    exec.runOne();                               // coalesced rebuild of the panel
    ui.runAll();
    const auto& optimistic = panel->state().get();
    EXPECT_EQ(optimistic.title, "Novo título");
    EXPECT_EQ(optimistic.description, "nova descrição");
    EXPECT_EQ(optimistic.labels.size(), 2u);
    EXPECT_EQ(optimistic.members.size(), 2u);
    ASSERT_FALSE(optimistic.comments.empty());
    EXPECT_EQ(optimistic.comments[0].text, "olá");
    EXPECT_TRUE(optimistic.comments[0].sending);

    pump();
    EXPECT_FALSE(panel->state().get().comments[0].sending);
    EXPECT_EQ(sent(Method::Post, "/cards/c1/idLabels").size(), 1u);
    EXPECT_EQ(sent(Method::Post, "/cards/c1/idMembers").size(), 1u);
    EXPECT_EQ(sent(Method::Post, "/cards/c1/actions/comments").size(), 1u);
}

TEST_F(VmTest, PanelShowsConflictNotice) {
    routeFirst(Method::Put, "/cards/c1", 200, R"({"id":"c1","idList":"l1","name":"x","pos":1})");
    routeFirst(Method::Get, "/cards/c1", 200,
               R"({"id":"c1","name":"x","desc":"descrição dela","dateLastActivity":"t7"})", "fields=name%2Cdesc%2CdateLastActivity");
    make();
    auto board = app->openBoard("b1");
    pump();
    auto panel = app->openCard("c1");
    pump();
    panel->setDescription("minha");
    pump();
    const auto& s = panel->state().get();
    ASSERT_FALSE(s.notices.empty());
    EXPECT_EQ(s.notices.back().kind, PanelNotice::Kind::ConflictOverwritten);
    EXPECT_EQ(s.notices.back().field, "desc");
    EXPECT_EQ(s.notices.back().detail, "descrição dela");

    panel->dismissNotices();
    pump();
    EXPECT_TRUE(panel->state().get().notices.empty());
}

TEST_F(VmTest, PanelShowsRejectionNoticeAndRevertsTitle) {
    routeFirst(Method::Put, "/cards/c1", 400, "invalid");
    make();
    auto board = app->openBoard("b1");
    pump();
    auto panel = app->openCard("c1");
    pump();
    panel->setTitle("recusado");
    pump();
    const auto& s = panel->state().get();
    EXPECT_EQ(s.title, "Corrigir login");
    ASSERT_FALSE(s.notices.empty());
    EXPECT_EQ(s.notices.back().kind, PanelNotice::Kind::WriteRejected);
    EXPECT_EQ(s.notices.back().field, "edit");
}

TEST_F(VmTest, PanelOpenedOnTemporaryCardFollowsItToTheRealId) {
    routeFirst(Method::Post, "/cards", 200, R"({"id":"c9","idList":"l1","name":"Novo","pos":65536})");
    routeFirst(Method::Get, "/cards/c9", 200,
               R"({"id":"c9","idList":"l1","name":"Novo","desc":"","badges":{"comments":0}})", "actions=commentCard");
    make();
    auto board = app->openBoard("b1");
    pump();
    board->createCard("l1", "Novo");
    exec.runOne();
    exec.runOne();
    ui.runAll();
    std::string tmp = board->column("l1")->cards.back().id;
    ASSERT_EQ(tmp.rfind("tmp-", 0), 0u);

    auto panel = app->openCard(tmp);
    pump();
    EXPECT_EQ(panel->state().get().cardId, "c9");
    EXPECT_EQ(panel->state().get().title, "Novo");
    EXPECT_EQ(panel->state().get().status, CardPanelState::Status::Ready);
}

TEST_F(VmTest, StatusTracksOfflinePendingWritesAndRecovery) {
    make();
    auto board = app->openBoard("b1");
    pump();
    EXPECT_FALSE(app->status().get().offline);

    req.failAll = true;
    auto panel = app->openCard("c1");
    panel->addLabel("lb2");
    pump();
    EXPECT_TRUE(app->status().get().offline);
    EXPECT_EQ(app->status().get().pendingWrites, 1);
    EXPECT_FALSE(app->status().get().loginRequired);

    req.failAll = false;
    routeFirst(Method::Post, "/cards/c1/idLabels", 200, "[]");
    app->retry();
    pump();
    EXPECT_FALSE(app->status().get().offline);
    EXPECT_EQ(app->status().get().pendingWrites, 0);
}

TEST_F(VmTest, StatusRequiresLoginOn401UntilLoginCompleted) {
    routeFirst(Method::Get, "/members/me/boards", 401, "invalid token");
    make();
    app->start();
    pump();
    EXPECT_TRUE(app->status().get().loginRequired);
    EXPECT_FALSE(app->status().get().offline);

    routeFirst(Method::Get, "/members/me/boards", 200, kBoards);
    app->loginCompleted();
    pump();
    EXPECT_FALSE(app->status().get().loginRequired);
    ASSERT_EQ(app->boardPicker()->state().get().boards.size(), 1u);
}

TEST_F(VmTest, DestroyedViewModelsIgnoreLateUpdates) {
    make();
    {
        auto board = app->openBoard("b1");
        auto panel = app->openCard("c1");
        exec.runOne();
    }  // UI dropped them while work is still queued
    EXPECT_NO_THROW(pump());
}

TEST_F(VmTest, AppModelCanBeDestroyedWithWorkStillQueued) {
    make();
    auto board = app->openBoard("b1");
    board.reset();
    app.reset();
    EXPECT_NO_THROW(pump());  // queued jobs and UI callbacks become no-ops
}

TEST_F(VmTest, BurstsOfChangesAreCoalescedIntoFewPublications) {
    make();
    auto board = app->openBoard("b1");
    pump();
    routeFirst(Method::Post, "/cards/c1/idLabels", 200, "[]");
    int published = 0;
    board->columnChanged.connect([&](const std::string&) { ++published; });
    auto panel = app->openCard("c1");
    pump();
    published = 0;
    for (int i = 0; i < 10; ++i) panel->addComment("n" + std::to_string(i));
    exec.runOne();  // run the ten enqueue jobs and whatever rebuilds they scheduled
    for (int i = 0; i < 9; ++i) exec.runOne();
    ui.runAll();
    EXPECT_LE(published, 2);
}

namespace {

// A dispatcher fed from many threads and drained by the test ("UI") thread.
struct ThreadSafeUi : UiDispatcher {
    void post(std::function<void()> fn) override {
        std::lock_guard lk(mu);
        queue.push_back(std::move(fn));
    }
    void runAll() {
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard lk(mu);
            batch.swap(queue);
        }
        for (auto& f : batch) f();
    }
    std::mutex mu;
    std::deque<std::function<void()>> queue;
};

}  // namespace

TEST_F(VmTest, RealThreadsLoadBoardAndPanelAndSendEditsWithoutRaces) {
    ThreadPoolExecutor pool(4);
    ThreadSafeUi tsUi;
    sync.prefetchCold = true;
    routeFirst(Method::Put, "/cards/c1", 200, R"({"id":"c1","idList":"l1","name":"x","pos":1})");
    routeFirst(Method::Post, "/cards/c1/actions/comments", 200,
               R"({"id":"a9","idMemberCreator":"m1","date":"d","data":{"text":"x"}})");
    {
        AppModel real(api, *store, pool, tsUi, &events, sync, write);
        real.start();
        auto board = real.openBoard("b1");
        for (int i = 0; i < 1000 && !(board->column("l1") && board->column("l1")->loaded); ++i) {
            tsUi.runAll();  // like a user, wait until the card is on screen before opening it
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_TRUE(board->column("l1") && board->column("l1")->loaded);
        auto panel = real.openCard("c1");
        for (int i = 0; i < 10; ++i) panel->addComment("n" + std::to_string(i));
        panel->setDescription("descrição final");

        auto settled = [&] {
            const auto cols = board->columns();
            return cols.size() == 3 && cols[2]->loaded && panel->state().get().description == "descrição final" &&
                   panel->state().get().extrasLoaded && real.status().get().pendingWrites == 0 &&
                   std::none_of(panel->state().get().comments.begin(), panel->state().get().comments.end(),
                                [](const CommentRow& c) { return c.sending; });  // all confirmed
        };
        for (int i = 0; i < 1000 && !settled(); ++i) {
            tsUi.runAll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        tsUi.runAll();
        EXPECT_TRUE(settled());
        EXPECT_EQ(sent(Method::Post, "/cards/c1/actions/comments").size(), 10u);
        board.reset();
        panel.reset();
    }  // AppModel destroyed while the pool keeps running
    tsUi.runAll();
}

TEST_F(VmTest, NetworkComingBackSendsPendingWritesAndClearsOfflineWithoutAnyUserAction) {
    make();
    auto board = app->openBoard("b1");
    pump();
    req.failAll = true;
    auto panel = app->openCard("c1");
    panel->addLabel("lb2");
    pump();
    ASSERT_TRUE(app->status().get().offline);
    ASSERT_EQ(app->status().get().pendingWrites, 1);

    req.failAll = false;
    routeFirst(Method::Post, "/cards/c1/idLabels", 200, "[]");
    app->onNetworkOk();  // e.g. the live-update poll got an answer
    pump();
    EXPECT_FALSE(app->status().get().offline);
    EXPECT_EQ(app->status().get().pendingWrites, 0);
    EXPECT_GE(sent(Method::Post, "/cards/c1/idLabels").size(), 2u);  // the failed try, then the real one
}

TEST_F(VmTest, NetworkComingBackAlsoReloadsTheBoardThatCouldNotLoad) {
    req.failAll = true;
    make();
    auto board = app->openBoard("b1");
    pump();
    EXPECT_TRUE(board->columns().empty());
    req.failAll = false;
    app->onNetworkOk();
    pump();
    EXPECT_EQ(board->columns().size(), 3u);
}

TEST_F(VmTest, PollErrorsAreReportedAsOfflineOrLoginRequired) {
    make();
    app->onNetworkError({0, "no route to host"});
    pump();
    EXPECT_TRUE(app->status().get().offline);
    app->onNetworkOk();
    pump();
    EXPECT_FALSE(app->status().get().offline);
    app->onNetworkError({401, "invalid token"});
    pump();
    EXPECT_TRUE(app->status().get().loginRequired);
}

TEST_F(VmTest, ConfirmedWriteProvesTheNetworkIsBack) {
    make();
    auto board = app->openBoard("b1");
    pump();
    app->onNetworkError({0, "offline"});
    pump();
    ASSERT_TRUE(app->status().get().offline);
    routeFirst(Method::Post, "/cards/c1/idLabels", 200, "[]");
    auto panel = app->openCard("c1");
    pump();
    panel->addLabel("lb2");
    pump();
    EXPECT_FALSE(app->status().get().offline);
}

TEST_F(VmTest, AListTheUserEmptiedStaysAnEmptyLoadedList) {
    make();
    auto board = app->openBoard("b1");
    board->expandList("l2");  // l2 holds only c3
    pump();
    routeFirst(Method::Put, "/cards/c3", 200, R"({"id":"c3","idList":"l1","name":"Migrar","pos":1.5})");
    board->moveCard("c3", "l1", 1);
    pump();
    auto l2 = board->column("l2");
    ASSERT_TRUE(l2);
    EXPECT_TRUE(l2->loaded);  // not a "1 card, not loaded yet" ghost
    EXPECT_TRUE(l2->cards.empty());
    EXPECT_EQ(l2->cardCount, 0);
}
