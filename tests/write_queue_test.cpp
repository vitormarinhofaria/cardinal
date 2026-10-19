#include <gtest/gtest.h>

#include <filesystem>
#include <mutex>

#include "cardinal/write_queue.h"
#include "fakes.h"

using namespace cardinal;
using namespace cardinal::testing;

namespace {

struct RecordingWriteSink : WriteSink {
    void onChanged(const ChangeSet& c) override {
        std::lock_guard lk(mu);
        changes.push_back(c);
    }
    void onError(const SyncError& e) override {
        std::lock_guard lk(mu);
        errors.push_back(e);
    }
    void onRejected(const Rejection& r) override {
        std::lock_guard lk(mu);
        rejected.push_back(r);
    }
    void onConflict(const Conflict& c) override {
        std::lock_guard lk(mu);
        conflicts.push_back(c);
    }
    bool any(std::vector<std::string> ChangeSet::*field, const std::string& id) {
        std::lock_guard lk(mu);
        for (const auto& c : changes)
            if (std::count((c.*field).begin(), (c.*field).end(), id)) return true;
        return false;
    }
    std::mutex mu;
    std::vector<ChangeSet> changes;
    std::vector<SyncError> errors;
    std::vector<Rejection> rejected;
    std::vector<Conflict> conflicts;
};

constexpr const char* kCard1 = R"({"id":"c1","idList":"l1","name":"A","desc":"old desc","pos":10,
    "dateLastActivity":"t0"})";

class QueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("cardinal-wq-" + std::string(::testing::UnitTest::GetInstance()
                                                 ->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        store = std::make_unique<Store>(Store::open(dir_ / "cache.db"));
        Card c1;
        c1.id = "c1";
        c1.listId = "l1";
        c1.name = "A";
        c1.desc = "old desc";
        c1.pos = 10;
        c1.dateLastActivity = "t0";
        Card c2;
        c2.id = "c2";
        c2.listId = "l1";
        c2.name = "B";
        c2.pos = 20;
        std::vector<Card> cards{c1, c2};
        store->upsertCards(cards);
        opts.now = [this] { return now; };
        opts.selfMemberId = "me";
    }
    void TearDown() override {
        queue.reset();
        store.reset();
        std::filesystem::remove_all(dir_);
    }
    void make() { queue = std::make_unique<WriteQueue>(api, *store, exec, sink, opts); }
    void route(Method m, const std::string& path, int status, const std::string& body,
               const std::string& contains = "") {
        req.routes.push_back({path, contains, {status, body}, m});
    }
    std::vector<Submitted> sent(Method m, const std::string& path) {
        std::vector<Submitted> out;
        for (const auto& c : req.calls)
            if (c.method == m && c.url.find("https://api.trello.com/1" + path + "?") == 0)
                out.push_back(c);
        return out;
    }

    std::filesystem::path dir_;
    std::unique_ptr<Store> store;
    RouteRequester req;
    TrelloApi api{req, "K", "T"};
    QueueExecutor exec;
    RecordingWriteSink sink;
    WriteOptions opts;
    std::time_t now = 1'760'875'500;
    std::unique_ptr<WriteQueue> queue;
};

}  // namespace

TEST_F(QueueTest, AppliesOptimisticallyThenConfirms) {
    route(Method::Put, "/cards/c1", 200,
          R"({"id":"c1","idList":"l2","name":"A","desc":"old desc","pos":5,"dateLastActivity":"t9"})");
    make();
    auto e = queue->enqueue(MoveCard{"c1", "l2", 5});
    EXPECT_EQ(e.cardId, "c1");

    // instantly, before any network
    EXPECT_EQ(store->card("c1")->listId, "l2");
    EXPECT_EQ(store->card("c1")->pos, 5);
    EXPECT_TRUE(sink.any(&ChangeSet::listCards, "l1"));
    EXPECT_TRUE(sink.any(&ChangeSet::listCards, "l2"));
    EXPECT_TRUE(req.calls.empty());
    EXPECT_EQ(store->pendingOps().size(), 1u);
    EXPECT_EQ(queue->pendingCount(), 1u);

    exec.drain();
    auto puts = sent(Method::Put, "/cards/c1");
    ASSERT_EQ(puts.size(), 1u);
    EXPECT_EQ(puts[0].priority, Priority::Interactive);
    EXPECT_EQ(parseForm(puts[0].body)["idList"], "l2");
    EXPECT_EQ(store->card("c1")->dateLastActivity, "t9");  // taken from the confirmation
    EXPECT_EQ(store->card("c1")->desc, "old desc");        // untouched
    EXPECT_TRUE(store->pendingOps().empty());
    EXPECT_EQ(queue->pendingCount(), 0u);
}

TEST_F(QueueTest, FailureRevertsAndNotifies) {
    route(Method::Put, "/cards/c1", 400, "invalid value");
    route(Method::Post, "/cards/c1/idLabels", 200, "[]");
    make();
    queue->enqueue(MoveCard{"c1", "l2", 5});
    queue->enqueue(AddLabel{"c1", "lb1"});
    exec.drain();

    EXPECT_EQ(store->card("c1")->listId, "l1");
    EXPECT_EQ(store->card("c1")->pos, 10);
    ASSERT_EQ(sink.rejected.size(), 1u);
    EXPECT_EQ(sink.rejected[0].status, 400);
    EXPECT_EQ(sink.rejected[0].kind, "move");
    EXPECT_EQ(sink.rejected[0].cardId, "c1");
    EXPECT_TRUE(store->pendingOps().empty());
    EXPECT_EQ(sent(Method::Post, "/cards/c1/idLabels").size(), 1u);  // the queue went on
    EXPECT_TRUE(sink.any(&ChangeSet::listCards, "l1"));
}

TEST_F(QueueTest, SurvivesRestart) {
    req.failAll = true;
    make();
    queue->enqueue(EditCard{"c1", std::nullopt, std::string("nova descrição")});
    exec.drain();
    EXPECT_EQ(store->pendingOps().size(), 1u);
    queue.reset();  // app closed

    req.failAll = false;
    route(Method::Get, "/cards/c1", 200, kCard1);
    route(Method::Put, "/cards/c1", 200,
          R"({"id":"c1","idList":"l1","name":"A","desc":"nova descrição","pos":10,"dateLastActivity":"t5"})");
    make();  // new process: loads the pending op from disk and starts sending
    EXPECT_EQ(queue->pendingCount(), 1u);
    EXPECT_EQ(store->card("c1")->desc, "nova descrição");  // still applied in the cache
    exec.drain();
    ASSERT_EQ(sent(Method::Put, "/cards/c1").size(), 1u);
    EXPECT_EQ(parseForm(sent(Method::Put, "/cards/c1")[0].body)["desc"], "nova descrição");
    EXPECT_TRUE(store->pendingOps().empty());
}

TEST_F(QueueTest, ConflictWarnsWhenLastActivityChangedAndFieldDiffers) {
    route(Method::Get, "/cards/c1", 200,
          R"({"id":"c1","name":"A","desc":"descrição dela","dateLastActivity":"t1"})");
    route(Method::Put, "/cards/c1", 200,
          R"({"id":"c1","idList":"l1","name":"A","desc":"minha","pos":10,"dateLastActivity":"t2"})");
    make();
    queue->enqueue(EditCard{"c1", std::nullopt, std::string("minha")});
    exec.drain();

    ASSERT_EQ(sink.conflicts.size(), 1u);
    EXPECT_EQ(sink.conflicts[0].cardId, "c1");
    EXPECT_EQ(sink.conflicts[0].field, "desc");
    EXPECT_EQ(sink.conflicts[0].theirs, "descrição dela");
    EXPECT_EQ(sink.conflicts[0].yours, "minha");
    EXPECT_EQ(sent(Method::Put, "/cards/c1").size(), 1u);  // last write wins
    EXPECT_EQ(store->card("c1")->desc, "minha");
}

TEST_F(QueueTest, NoConflictWhenOnlyActivityChangedNotTheEditedField) {
    route(Method::Get, "/cards/c1", 200,
          R"({"id":"c1","name":"A renomeado?","desc":"old desc","dateLastActivity":"t1"})");
    route(Method::Put, "/cards/c1", 200, kCard1);
    make();
    queue->enqueue(EditCard{"c1", std::nullopt, std::string("minha")});  // edits desc only
    exec.drain();
    EXPECT_TRUE(sink.conflicts.empty());
}

TEST_F(QueueTest, NoConflictWhenActivityIsUnchanged) {
    route(Method::Get, "/cards/c1", 200,
          R"({"id":"c1","name":"A","desc":"qualquer coisa","dateLastActivity":"t0"})");
    route(Method::Put, "/cards/c1", 200, kCard1);
    make();
    queue->enqueue(EditCard{"c1", std::nullopt, std::string("minha")});
    exec.drain();
    EXPECT_TRUE(sink.conflicts.empty());
}

TEST_F(QueueTest, OfflineKeepsOpsUntilReconnect) {
    req.failAll = true;
    make();
    queue->enqueue(AddLabel{"c1", "lb1"});
    queue->enqueue(AddMember{"c1", "m1"});
    exec.drain();

    EXPECT_EQ(queue->pendingCount(), 2u);
    EXPECT_EQ(store->pendingOps().size(), 2u);
    ASSERT_EQ(sink.errors.size(), 1u);  // one report per attempt, not per op
    EXPECT_EQ(sink.errors[0].status, 0);
    EXPECT_TRUE(sink.rejected.empty());
    EXPECT_EQ(store->card("c1")->labelIds, (std::vector<std::string>{"lb1"}));  // optimistic stays
    EXPECT_EQ(store->card("c1")->memberIds, (std::vector<std::string>{"m1"}));

    req.failAll = false;
    req.calls.clear();
    route(Method::Post, "/cards/c1/idLabels", 200, "[]");
    route(Method::Post, "/cards/c1/idMembers", 200, "[]");
    queue->flush();
    exec.drain();
    EXPECT_EQ(queue->pendingCount(), 0u);
    ASSERT_EQ(req.calls.size(), 2u);
    EXPECT_NE(req.calls[0].url.find("/idLabels"), std::string::npos);  // original order
    EXPECT_NE(req.calls[1].url.find("/idMembers"), std::string::npos);
}

TEST_F(QueueTest, TransientFailureStopsLaterOpsFromJumpingAhead) {
    route(Method::Put, "/cards/c1", 503, "unavailable");
    route(Method::Post, "/cards/c1/idLabels", 200, "[]");
    make();
    queue->enqueue(EditCard{"c1", std::string("novo nome"), std::nullopt});
    queue->enqueue(AddLabel{"c1", "lb1"});
    route(Method::Get, "/cards/c1", 200, kCard1);
    exec.drain();
    EXPECT_TRUE(sent(Method::Post, "/cards/c1/idLabels").empty());  // waits behind the edit
    EXPECT_EQ(queue->pendingCount(), 2u);
    ASSERT_EQ(sink.errors.size(), 1u);
    EXPECT_EQ(sink.errors[0].status, 503);
}

TEST_F(QueueTest, UnauthorizedKeepsOpsPendingAndReportsStatus) {
    route(Method::Post, "/cards/c1/idLabels", 401, "invalid token");
    make();
    queue->enqueue(AddLabel{"c1", "lb1"});
    exec.drain();
    EXPECT_EQ(queue->pendingCount(), 1u);
    ASSERT_EQ(sink.errors.size(), 1u);
    EXPECT_EQ(sink.errors[0].status, 401);
    EXPECT_TRUE(sink.rejected.empty());
    EXPECT_EQ(store->card("c1")->labelIds, (std::vector<std::string>{"lb1"}));  // not reverted
}

TEST_F(QueueTest, PreservesOrderPerCard) {
    route(Method::Get, "/cards/c1", 200, kCard1);
    route(Method::Put, "/cards/c1", 200, kCard1);
    route(Method::Post, "/cards/c1/actions/comments", 200,
          R"({"id":"a1","idMemberCreator":"me","date":"d","data":{"text":"oi"}})");
    make();
    queue->enqueue(EditCard{"c1", std::string("n1"), std::nullopt});
    queue->enqueue(MoveCard{"c1", "l2", 1});
    queue->enqueue(AddComment{"c1", "oi"});
    exec.drain();

    std::vector<std::string> writes;
    for (const auto& c : req.calls) {
        if (c.method == Method::Get) continue;
        auto form = parseForm(c.body);
        writes.push_back(c.method == Method::Put ? (form.count("name") ? "edit" : "move")
                                                  : "comment");
    }
    EXPECT_EQ(writes, (std::vector<std::string>{"edit", "move", "comment"}));
}

TEST_F(QueueTest, CreateCardShowsTemporaryCardThenSwapsForTheRealOne) {
    route(Method::Post, "/cards", 200,
          R"({"id":"c9","idList":"l1","name":"Novo","pos":65536,"dateLastActivity":"t3"})");
    make();
    auto e = queue->enqueue(CreateCard{"l1", "Novo"});
    EXPECT_EQ(e.cardId.rfind("tmp-", 0), 0u);
    ASSERT_TRUE(store->card(e.cardId).has_value());
    EXPECT_EQ(store->card(e.cardId)->name, "Novo");
    EXPECT_GT(store->card(e.cardId)->pos, 20);  // appended after the existing cards

    exec.drain();
    EXPECT_FALSE(store->card(e.cardId).has_value());
    ASSERT_TRUE(store->card("c9").has_value());
    EXPECT_EQ(store->card("c9")->listId, "l1");
    ASSERT_EQ(sink.changes.back().renamedCards.size() + (sink.changes.size() > 1 ? 0 : 0), 1u);
    bool renamed = false;
    for (const auto& c : sink.changes)
        for (const auto& [from, to] : c.renamedCards) renamed |= from == e.cardId && to == "c9";
    EXPECT_TRUE(renamed);
}

TEST_F(QueueTest, OpsQueuedOnTemporaryCardAreRewrittenToTheRealId) {
    route(Method::Post, "/cards", 200, R"({"id":"c9","idList":"l1","name":"Novo","pos":65536})");
    route(Method::Get, "/cards/c9", 200, R"({"id":"c9","name":"Novo","desc":"","dateLastActivity":""})");
    route(Method::Put, "/cards/c9", 200, R"({"id":"c9","idList":"l1","name":"Renomeado","pos":65536})");
    make();
    auto e = queue->enqueue(CreateCard{"l1", "Novo"});
    queue->enqueue(EditCard{e.cardId, std::string("Renomeado"), std::nullopt});  // before it exists
    exec.drain();

    EXPECT_EQ(sent(Method::Put, "/cards/c9").size(), 1u);
    for (const auto& c : req.calls) EXPECT_EQ(c.url.find("tmp-"), std::string::npos) << c.url;
    EXPECT_EQ(store->card("c9")->name, "Renomeado");
    EXPECT_TRUE(store->pendingOps().empty());
}

TEST_F(QueueTest, CreateCardFailureRemovesTheTemporaryCard) {
    route(Method::Post, "/cards", 400, "bad");
    make();
    auto e = queue->enqueue(CreateCard{"l1", "Novo"});
    exec.drain();
    EXPECT_FALSE(store->card(e.cardId).has_value());
    ASSERT_EQ(sink.rejected.size(), 1u);
    EXPECT_EQ(sink.rejected[0].kind, "create");
    EXPECT_TRUE(sink.any(&ChangeSet::listCards, "l1"));
}

TEST_F(QueueTest, CommentAppearsAtOnceThenIsReplacedByTheRealOne) {
    route(Method::Post, "/cards/c1/actions/comments", 200,
          R"({"id":"a77","idMemberCreator":"me","date":"2026-10-19T12:06:00.000Z","data":{"text":"olá 👋"}})");
    std::vector<Comment> existing{{"a1", "m2", "antigo", "d"}};
    store->replaceComments("c1", existing);
    make();
    queue->enqueue(AddComment{"c1", "olá 👋"});

    auto mine = store->comments("c1");
    ASSERT_EQ(mine.size(), 2u);
    EXPECT_EQ(mine[0].text, "olá 👋");  // newest first
    EXPECT_EQ(mine[0].authorId, "me");
    EXPECT_EQ(mine[0].id.rfind("tmp-", 0), 0u);
    EXPECT_EQ(store->card("c1")->commentCount, 1);
    EXPECT_TRUE(sink.any(&ChangeSet::cardDetails, "c1"));

    exec.drain();
    auto after = store->comments("c1");
    ASSERT_EQ(after.size(), 2u);
    EXPECT_EQ(after[0].id, "a77");
    EXPECT_EQ(after[1].id, "a1");
    EXPECT_EQ(parseForm(sent(Method::Post, "/cards/c1/actions/comments")[0].body)["text"], "olá 👋");
}

TEST_F(QueueTest, CommentFailureRemovesIt) {
    route(Method::Post, "/cards/c1/actions/comments", 400, "bad");
    make();
    queue->enqueue(AddComment{"c1", "x"});
    exec.drain();
    EXPECT_TRUE(store->comments("c1").empty());
    EXPECT_EQ(store->card("c1")->commentCount, 0);
    EXPECT_EQ(sink.rejected.size(), 1u);
}

TEST_F(QueueTest, LabelAndMemberAreAppliedOnceEvenIfEnqueuedTwice) {
    route(Method::Post, "/cards/c1/idLabels", 200, "[]");
    make();
    queue->enqueue(AddLabel{"c1", "lb1"});
    queue->enqueue(AddLabel{"c1", "lb1"});
    EXPECT_EQ(store->card("c1")->labelIds, (std::vector<std::string>{"lb1"}));
}

TEST_F(QueueTest, LabelFailureRevertsToPreviousLabels) {
    route(Method::Post, "/cards/c1/idLabels", 400, "bad");
    std::vector<Card> c{*store->card("c1")};
    c[0].labelIds = {"keep"};
    store->upsertCards(c);
    make();
    queue->enqueue(AddLabel{"c1", "lb1"});
    EXPECT_EQ(store->card("c1")->labelIds, (std::vector<std::string>{"keep", "lb1"}));
    exec.drain();
    EXPECT_EQ(store->card("c1")->labelIds, (std::vector<std::string>{"keep"}));
}

TEST_F(QueueTest, EditFailureRevertsNameAndDescription) {
    route(Method::Get, "/cards/c1", 200, kCard1);
    route(Method::Put, "/cards/c1", 422, "nope");
    make();
    queue->enqueue(EditCard{"c1", std::string("N"), std::string("D")});
    EXPECT_EQ(store->card("c1")->name, "N");
    exec.drain();
    EXPECT_EQ(store->card("c1")->name, "A");
    EXPECT_EQ(store->card("c1")->desc, "old desc");
}

TEST_F(QueueTest, CardDeletedRemotelyIsRemovedAndReported) {
    route(Method::Put, "/cards/c1", 404, "gone");
    make();
    queue->enqueue(MoveCard{"c1", "l2", 1});
    exec.drain();
    EXPECT_FALSE(store->card("c1").has_value());
    EXPECT_TRUE(sink.any(&ChangeSet::removedCards, "c1"));
    ASSERT_EQ(sink.rejected.size(), 1u);
    EXPECT_EQ(sink.rejected[0].status, 404);
    EXPECT_TRUE(store->pendingOps().empty());
}

TEST_F(QueueTest, EnqueueOnUnknownCardThrowsAndLeavesNothingBehind) {
    make();
    EXPECT_THROW(queue->enqueue(MoveCard{"nope", "l2", 1}), std::invalid_argument);
    EXPECT_TRUE(store->pendingOps().empty());
    EXPECT_TRUE(exec.jobs.empty());
}

TEST_F(QueueTest, ExtraFlushesDoNotSendAnythingTwice) {
    route(Method::Post, "/cards/c1/idLabels", 200, "[]");
    route(Method::Post, "/cards/c1/idMembers", 200, "[]");
    make();
    queue->enqueue(AddLabel{"c1", "lb1"});
    queue->enqueue(AddMember{"c1", "m1"});
    queue->flush();
    queue->flush();
    exec.drain();
    EXPECT_EQ(req.calls.size(), 2u);
}

TEST_F(QueueTest, JobsQueuedBeforeDestructionBecomeNoOps) {
    route(Method::Post, "/cards/c1/idLabels", 200, "[]");
    make();
    queue->enqueue(AddLabel{"c1", "lb1"});
    queue.reset();
    exec.drain();
    EXPECT_TRUE(req.calls.empty());
    EXPECT_EQ(store->pendingOps().size(), 1u);  // still pending for the next start
}

TEST_F(QueueTest, ConcurrentEnqueuesAreAllSentExactlyOnceInOrder) {
    ThreadPoolExecutor pool(4);
    route(Method::Post, "/cards/c1/actions/comments", 200,
          R"({"id":"a1","idMemberCreator":"me","date":"d","data":{"text":"x"}})");
    {
        WriteQueue real(api, *store, pool, sink, opts);
        for (int i = 0; i < 40; ++i) real.enqueue(AddComment{"c1", "n" + std::to_string(i)});
        for (int i = 0; i < 500 && real.pendingCount() > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        EXPECT_EQ(real.pendingCount(), 0u);
    }
    auto posts = sent(Method::Post, "/cards/c1/actions/comments");
    ASSERT_EQ(posts.size(), 40u);
    for (int i = 0; i < 40; ++i) EXPECT_EQ(parseForm(posts[i].body)["text"], "n" + std::to_string(i));
}
