#include <gtest/gtest.h>

#include <filesystem>
#include <thread>

#include "cardinal/app_context.h"
#include "fakes.h"
#include "http_server.h"

using namespace cardinal;
using namespace cardinal::testing;
using namespace std::chrono_literals;

namespace {

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

// A pretend Trello: a board with two lists, card c1 with a comment.
HttpReply fakeTrello(const HttpRequestInfo& r) {
    const std::string& p = r.path;
    auto has = [&](const char* s) { return r.query.find(s) != std::string::npos; };
    if (r.query.find("key=KEY") == std::string::npos || r.query.find("token=TOKEN") == std::string::npos)
        return {401, "invalid key"};
    if (p == "/1/members/me/boards") return {200, R"([{"id":"b1","name":"Produto","closed":false}])"};
    if (p == "/1/boards/b1/lists")
        return {200, R"([{"id":"l1","name":"A fazer","pos":1,"cards":[{"id":"c1"}]},
                         {"id":"l2","name":"Feito","pos":2,"cards":[]}])"};
    if (p == "/1/boards/b1")
        return {200, R"({"id":"b1","name":"Produto","labels":[{"id":"lb1","name":"Bug","color":"red"}],
                         "members":[{"id":"m1","fullName":"Vitor M","initials":"VM"}]})"};
    if (p == "/1/lists/l1/cards")
        return {200, R"([{"id":"c1","idList":"l1","name":"Corrigir login","pos":1,"idLabels":["lb1"]}])"};
    if (p == "/1/lists/l2/cards") return {200, "[]"};
    if (p == "/1/boards/b1/actions") return {200, "[]"};
    if (p == "/1/cards/c1" && r.method == "GET" && has("checklists=all"))
        return {200, R"({"id":"c1","checklists":[],"attachments":[],"members":[]})"};
    if (p == "/1/cards/c1" && r.method == "GET")
        return {200, R"({"id":"c1","idList":"l1","name":"Corrigir login","desc":"descrição 🚀","pos":1,
                         "actions":[{"id":"a1","idMemberCreator":"m1","date":"d","data":{"text":"primeiro"}}]})"};
    if (p == "/1/cards/c1/actions/comments" && r.method == "POST")
        return {200, R"({"id":"a2","idMemberCreator":"m1","date":"d2","data":{"text":"olá"}})"};
    return {404, "not found"};
}

}  // namespace

TEST(Integration, BoardCardAndCommentOverRealHttp) {
    RoutedServer server(fakeTrello);
    auto dir = std::filesystem::temp_directory_path() / "cardinal-integration";
    std::filesystem::remove_all(dir);

    ThreadSafeUi ui;
    AppConfig cfg;
    cfg.dataDir = dir;
    cfg.apiKey = "KEY";
    cfg.token = "TOKEN";
    cfg.apiBase = server.url() + "/1";
    cfg.pollInterval = 50ms;
    cfg.sync.alwaysHotLists = 2;
    auto settle = [&](auto cond) {
        for (int i = 0; i < 1000 && !cond(); ++i) {
            ui.runAll();
            std::this_thread::sleep_for(5ms);
        }
        ui.runAll();
        return cond();
    };
    {
        AppContext app(cfg, ui);
        app.model().start();
        auto board = app.model().openBoard("b1");
        ASSERT_TRUE(settle([&] {
            auto l1 = board->column("l1");
            return l1 && l1->loaded && l1->cards.size() == 1;
        }));
        EXPECT_EQ(board->header().get().name, "Produto");
        EXPECT_EQ(board->column("l1")->cards[0].labels[0].name, "Bug");
        ASSERT_EQ(app.model().boardPicker()->state().get().boards.size(), 1u);

        auto panel = app.model().openCard("c1");
        ASSERT_TRUE(settle([&] { return panel->state().get().description == "descrição 🚀"; }));
        EXPECT_EQ(panel->state().get().comments[0].text, "primeiro");

        panel->addComment("olá");
        ASSERT_TRUE(settle([&] { return server.count("POST", "/1/cards/c1/actions/comments") == 1; }));
        ASSERT_TRUE(settle([&] { return app.model().status().get().pendingWrites == 0; }));
        auto posts = server.requests();
        std::string body;
        for (const auto& r : posts)
            if (r.method == "POST") body = r.body;
        EXPECT_EQ(parseForm(body)["text"], "olá");

        // live updates keep polling in the background
        ASSERT_TRUE(settle([&] { return server.count("GET", "/1/boards/b1/actions") >= 2; }));
        board.reset();
        panel.reset();
    }
    EXPECT_TRUE(std::filesystem::exists(dir / "cache.db"));

    // a second start shows the cache before the network answers
    {
        AppContext app(cfg, ui);
        auto board = app.model().openBoard("b1");
        ASSERT_TRUE(settle([&] { return board->column("l1") && board->column("l1")->loaded; }));
        EXPECT_EQ(board->column("l1")->cards[0].title, "Corrigir login");
        board.reset();
    }
    std::filesystem::remove_all(dir);
}

TEST(Integration, WrongTokenSurfacesAsLoginRequired) {
    RoutedServer server(fakeTrello);
    auto dir = std::filesystem::temp_directory_path() / "cardinal-integration-401";
    std::filesystem::remove_all(dir);
    ThreadSafeUi ui;
    AppConfig cfg;
    cfg.dataDir = dir;
    cfg.apiKey = "KEY";
    cfg.token = "WRONG";
    cfg.apiBase = server.url() + "/1";
    {
        AppContext app(cfg, ui);
        app.model().start();
        for (int i = 0; i < 1000 && !app.model().status().get().loginRequired; ++i) {
            ui.runAll();
            std::this_thread::sleep_for(5ms);
        }
        EXPECT_TRUE(app.model().status().get().loginRequired);
    }
    std::filesystem::remove_all(dir);
}
