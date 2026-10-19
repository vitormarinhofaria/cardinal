// Latency numbers for the core, with a 2000-card board. Not part of ctest:
//   cmake --build build --target core_bench && build/tests/bench/core_bench
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <thread>

#include <nlohmann/json.hpp>

#include "../fakes.h"
#include "../http_server.h"
#include "cardinal/app_context.h"

using namespace cardinal;
using namespace cardinal::testing;
using Clock_ = std::chrono::steady_clock;
using namespace std::chrono_literals;
using nlohmann::json;

namespace {

constexpr int kLists = 12;
constexpr int kCards = 2000;

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

struct Fixture {
    json labels = json::array(), members = json::array(), lists = json::array();
    std::vector<json> cardsByList;
    json card(int i, int list) const {
        return {{"id", "c" + std::to_string(i)}, {"idList", "l" + std::to_string(list)},
                {"name", "Card número " + std::to_string(i) + " com um título razoavelmente comprido"},
                {"desc", "Descrição do card " + std::to_string(i)}, {"pos", i * 65536.0},
                {"idLabels", json::array({"lb" + std::to_string(i % 5)})},
                {"idMembers", json::array({"m" + std::to_string(i % 4)})},
                {"badges", {{"comments", i % 5}}}, {"dateLastActivity", "2026-10-19T12:00:00.000Z"}};
    }
    Fixture() {
        for (int i = 0; i < 8; ++i) labels.push_back({{"id", "lb" + std::to_string(i)}, {"name", "L" + std::to_string(i)}, {"color", "green"}});
        for (int i = 0; i < 6; ++i) members.push_back({{"id", "m" + std::to_string(i)}, {"fullName", "Pessoa " + std::to_string(i)}, {"initials", "P"}});
        int n = 0;
        cardsByList.resize(kLists);
        for (int l = 0; l < kLists; ++l) {
            const int size = l < 4 ? 12 : (kCards - 48) / (kLists - 4);
            json ids = json::array();
            for (int k = 0; k < size; ++k, ++n) {
                cardsByList[l].push_back(card(n, l));
                ids.push_back({{"id", "c" + std::to_string(n)}});
            }
            lists.push_back({{"id", "l" + std::to_string(l)}, {"name", "Lista " + std::to_string(l)}, {"pos", l + 1.0}, {"cards", ids}});
        }
    }
};

HttpReply serve(const Fixture& f, const HttpRequestInfo& r) {
    if (r.path == "/1/members/me/boards") return {200, R"([{"id":"b1","name":"Produto","closed":false}])"};
    if (r.path == "/1/boards/b1") return {200, json{{"id", "b1"}, {"name", "Produto"}, {"labels", f.labels}, {"members", f.members}}.dump()};
    if (r.path == "/1/boards/b1/lists") return {200, f.lists.dump()};
    if (r.path == "/1/boards/b1/actions") return {200, "[]"};
    if (r.path.rfind("/1/lists/l", 0) == 0) {
        int l = std::stoi(r.path.substr(10, r.path.find('/', 10) - 10));
        return {200, json(f.cardsByList.at(static_cast<size_t>(l))).dump()};
    }
    if (r.path.rfind("/1/cards/c", 0) == 0) {
        int i = std::stoi(r.path.substr(10));
        json c = f.card(i, 0);
        if (r.query.find("checklists=all") != std::string::npos) return {200, R"({"checklists":[],"attachments":[],"members":[]})"};
        c["actions"] = json::array();
        return {200, c.dump()};
    }
    return {404, "not found"};
}

double ms(Clock_::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

template <class F>
bool waitUntil(ThreadSafeUi& ui, F cond, std::chrono::seconds limit = 30s) {
    const auto end = Clock_::now() + limit;
    while (Clock_::now() < end) {
        ui.runAll();
        if (cond()) return true;
        std::this_thread::yield();
    }
    return false;
}

void report(const char* name, std::vector<double> v) {
    std::sort(v.begin(), v.end());
    std::printf("  %-52s median %7.2f ms   p95 %7.2f ms   max %7.2f ms   (n=%zu)\n", name, v[v.size() / 2],
                v[static_cast<size_t>(static_cast<double>(v.size()) * 0.95)], v.back(), v.size());
}

}  // namespace

int main() {
    Fixture fixture;
    RoutedServer server([&](const HttpRequestInfo& r) { return serve(fixture, r); });
    auto dir = std::filesystem::temp_directory_path() / "cardinal-bench";
    std::filesystem::remove_all(dir);
    ThreadSafeUi ui;
    AppConfig cfg;
    cfg.dataDir = dir;
    cfg.apiKey = "K";
    cfg.token = "T";
    cfg.apiBase = server.url() + "/1";
    cfg.pollInterval = 1h;

    std::printf("Cardinal core benchmark: %d cards in %d lists, loopback HTTP server\n\n", kCards, kLists);

    // ---- cold: empty cache, everything over HTTP
    {
        auto t0 = Clock_::now();
        AppContext app(cfg, ui);
        app.model().start();
        auto board = app.model().openBoard("b1");
        double firstCards = 0, allLists = 0;
        waitUntil(ui, [&] {
            auto c = board->column("l0");
            if (c && c->loaded && !c->cards.empty() && firstCards == 0) firstCards = ms(Clock_::now() - t0);
            return firstCards > 0;
        });
        waitUntil(ui, [&] {
            for (const auto& c : board->columns())
                if (!c->loaded) return false;
            return board->columns().size() == static_cast<size_t>(kLists);
        });
        allLists = ms(Clock_::now() - t0);
        std::printf("Cold start (empty cache):\n");
        std::printf("  first column with cards on screen                    %7.1f ms\n", firstCards);
        std::printf("  all %d lists / %d cards loaded                       %7.1f ms\n", kLists, kCards, allLists);
        std::printf("  HTTP requests until then: %zu  (board 2, lists %d, boards 1)\n\n", server.requests().size(), kLists);
        board.reset();
    }

    // ---- warm: cache on disk, network unreachable (the cache alone must carry the UI)
    {
        std::vector<double> boardOpen, cardOpen, cardOpenWithExtras;
        const size_t reqBefore = server.requests().size();
        for (int round = 0; round < 15; ++round) {
            AppConfig offline = cfg;
            offline.apiBase = "http://127.0.0.1:1/1";  // nothing listens: only the cache answers
            AppContext app(offline, ui);
            auto t0 = Clock_::now();
            auto board = app.model().openBoard("b1");
            waitUntil(ui, [&] {
                auto c = board->column("l0");
                return c && c->loaded && !c->cards.empty();
            });
            boardOpen.push_back(ms(Clock_::now() - t0));
            waitUntil(ui, [&] { return board->columns().size() == static_cast<size_t>(kLists) && board->column("l11") && board->column("l11")->loaded; });

            for (int k = 0; k < 8; ++k) {
                const std::string id = "c" + std::to_string(k * 3);
                auto t1 = Clock_::now();
                auto panel = app.model().openCard(id);
                waitUntil(ui, [&] { return panel->state().get().status == CardPanelState::Status::Ready; });
                cardOpen.push_back(ms(Clock_::now() - t1));
            }
            board.reset();
        }
        std::printf("Warm start (cache on disk, network down):\n");
        report("board: openBoard() -> first column with cards", boardOpen);
        report("card:  openCard() -> panel state with description", cardOpen);
        std::printf("  requests sent to the server while offline: %zu\n\n", server.requests().size() - reqBefore);
    }

    // ---- warm cache, network up: how soon is the fresh card in the panel
    {
        std::vector<double> fresh;
        for (int round = 0; round < 10; ++round) {
            AppContext app(cfg, ui);
            auto board = app.model().openBoard("b1");
            waitUntil(ui, [&] { auto c = board->column("l0"); return c && c->loaded; });
            auto t0 = Clock_::now();
            auto panel = app.model().openCard("c" + std::to_string(round));
            waitUntil(ui, [&] { return panel->state().get().extrasLoaded; });
            fresh.push_back(ms(Clock_::now() - t0));
            board.reset();
        }
        std::printf("Warm cache, network up:\n");
        report("card:  openCard() -> description+comments+extras all fresh", fresh);
    }
    std::filesystem::remove_all(dir);
    return 0;
}
