#include "cardinal/sync.h"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>

#include "cardinal/time_util.h"

namespace cardinal {

struct SyncEngine::Impl : std::enable_shared_from_this<Impl> {
    Impl(TrelloApi& a, Store& s, Executor& e, ChangeSink& k, SyncOptions o, EventSource* ev)
        : api(a), store(s), exec(e), sink(k), opts(std::move(o)), events(ev) {}

    TrelloApi& api;
    Store& store;
    Executor& exec;
    ChangeSink& sink;
    const SyncOptions opts;
    EventSource* const events;

    std::mutex mutex;
    std::condition_variable idle;
    bool closed = false;
    int running = 0;
    std::string currentBoard;
    std::string openCardId;
    std::map<std::string, CardExtras> extras;

    // Jobs hold the Impl alive, so a job queued before the engine was destroyed stays safe to run.
    void post(std::function<void()> job) {
        exec.post([self = shared_from_this(), job = std::move(job)] { self->run(job); });
    }

    void run(const std::function<void()>& job) {
        {
            std::lock_guard lk(mutex);
            if (closed) return;
            ++running;
        }
        try {
            job();
        } catch (const ApiError& e) {
            sink.onError({e.status(), e.what()});
        } catch (const std::exception& e) {
            sink.onError({0, e.what()});
        }
        std::lock_guard lk(mutex);
        --running;
        idle.notify_all();
    }

    void shutdown() {
        std::unique_lock lk(mutex);
        closed = true;
        idle.wait(lk, [this] { return running == 0; });
    }

    bool isCurrent(const std::string& boardId) {
        std::lock_guard lk(mutex);
        return currentBoard == boardId;
    }

    bool isHot(const List& list, int index) {
        if (index < opts.alwaysHotLists) return true;
        if (list.cardCount <= opts.smallListThreshold) return true;
        auto touched = store.listTouchedAt(list.id);
        if (!touched) return false;
        return *touched >= opts.now() - std::chrono::seconds(opts.hotWindow).count();
    }

    void loadBoards() {
        auto boards = api.boards().get();
        std::vector<std::string> ids;
        for (const auto& b : boards) ids.push_back(b.id);
        store.upsertBoards(boards);
        store.pruneBoards(ids);
        ChangeSet c;
        c.boards = true;
        sink.onChanged(c);
    }

    // `fresh`: the user just opened the board, so live updates start from now. Otherwise this is a
    // refresh triggered by an event and the cursor must stay where the poller put it.
    void loadBoard(const std::string& boardId, bool fresh) {
        if (fresh && events) {
            store.setSyncCursor(boardId, isoUtc(opts.now()));
            events->watch(boardId, [weak = std::weak_ptr<Impl>(shared_from_this()),
                                    boardId](std::vector<Action> actions) {
                if (auto self = weak.lock()) self->applyActions(boardId, actions);
            });
            if (!isCurrent(boardId)) events->unwatch(boardId);  // user already moved on
        }
        auto snap = api.board(boardId, Priority::Visible).get();
        std::vector<std::string> listIds;
        for (const auto& l : snap.lists) listIds.push_back(l.id);
        store.upsertLists(snap.lists);
        store.pruneBoardLists(boardId, listIds);
        store.replaceLabels(boardId, snap.labels);
        store.replaceMembers(boardId, snap.members);
        ChangeSet c;
        c.boardLists = {boardId};
        sink.onChanged(c);

        std::vector<std::string> cold;
        for (size_t i = 0; i < snap.lists.size(); ++i) {
            const auto& l = snap.lists[i];
            if (isHot(l, static_cast<int>(i))) {
                post([this, id = l.id] { loadListCards(id, Priority::Visible); });
            } else {
                cold.push_back(l.id);
            }
        }
        if (opts.prefetchCold && !cold.empty())
            post([this, boardId, cold] { prefetchCold(boardId, cold, 0); });
    }

    void loadListCards(const std::string& listId, Priority priority) {
        auto cards = api.listCards(listId, priority).get();
        std::vector<std::string> ids;
        for (const auto& c : cards) ids.push_back(c.id);
        store.upsertCardSummaries(cards);
        store.pruneListCards(listId, ids);
        ChangeSet c;
        c.listCards = {listId};
        sink.onChanged(c);
    }

    // One list per job: a long prefetch never occupies more than one pool thread.
    void prefetchCold(const std::string& boardId, const std::vector<std::string>& ids, size_t i) {
        if (i >= ids.size() || !isCurrent(boardId)) return;
        loadListCards(ids[i], Priority::Background);
        post([this, boardId, ids, i] { prefetchCold(boardId, ids, i + 1); });
    }

    void expandList(const std::string& listId) {
        store.touchList(listId, opts.now());
        loadListCards(listId, Priority::Interactive);
    }

    void removeCard(const std::string& cardId, const std::optional<Card>& cached) {
        store.deleteCard(cardId);
        {
            std::lock_guard lk(mutex);
            extras.erase(cardId);
        }
        ChangeSet c;
        c.removedCards = {cardId};
        if (cached) c.listCards = {cached->listId};
        sink.onChanged(c);
    }

    void openCard(const std::string& cardId) {
        {
            std::lock_guard lk(mutex);
            openCardId = cardId;
        }
        auto cached = store.card(cardId);
        if (cached) store.touchList(cached->listId, opts.now());

        CardCore core;
        try {
            core = api.cardCore(cardId).get();
        } catch (const ApiError& e) {
            if (e.status() != 404) throw;
            removeCard(cardId, cached);
            return;
        }
        if (core.card.closed) {
            removeCard(cardId, cached);
            return;
        }
        std::vector<Card> one{core.card};
        store.upsertCards(one);
        store.replaceComments(cardId, core.comments);
        ChangeSet c;
        c.cardDetails = {cardId};
        if (cached && cached->listId != core.card.listId) {
            c.listCards = {core.card.listId, cached->listId};
        }
        sink.onChanged(c);

        post([this, cardId] { loadExtras(cardId); });
    }


    // Live updates: invalidate and refetch what the actions touched. Runs on the poller's thread
    // and throws on failure, so the poller keeps its cursor and retries the batch.
    void applyActions(const std::string& boardId, const std::vector<Action>& actions) {
        {
            std::lock_guard lk(mutex);
            if (closed || currentBoard != boardId) return;
            ++running;
        }
        struct Done {
            Impl& self;
            ~Done() {
                std::lock_guard lk(self.mutex);
                --self.running;
                self.idle.notify_all();
            }
        } done{*this};

        std::string open;
        {
            std::lock_guard lk(mutex);
            open = openCardId;
        }
        std::set<std::string> lists, removed;
        bool boardChanged = actions.size() >= kActionLimit;  // older events may be missing
        bool refreshOpen = false;

        for (const auto& a : actions) {
            auto data = nlohmann::json::parse(a.dataJson.empty() ? "{}" : a.dataJson, nullptr, false);
            if (!data.is_object()) data = nlohmann::json::object();
            auto idOf = [&](const char* key) -> std::string {
                auto it = data.find(key);
                return it != data.end() && it->is_object() ? it->value("id", std::string()) : "";
            };
            auto addList = [&](const std::string& id) {
                if (!id.empty()) lists.insert(id);
            };
            const std::string& type = a.type;
            if (type == "createList" || type == "updateList") {
                boardChanged = true;
                continue;
            }
            if (!a.cardId.empty() && a.cardId == open) refreshOpen = true;

            bool archived = type == "updateCard" && data.contains("card") &&
                            data["card"].is_object() && data["card"].value("closed", false);
            if (type == "deleteCard" || type == "moveCardFromBoard" || archived) {
                if (!a.cardId.empty()) removed.insert(a.cardId);
                addList(idOf("list"));
                addList(idOf("listBefore"));
                continue;
            }
            addList(idOf("list"));
            addList(idOf("listAfter"));
            addList(idOf("listBefore"));
            if (idOf("list").empty() && idOf("listAfter").empty()) {
                if (auto cached = store.card(a.cardId)) addList(cached->listId);
            }
        }

        if (boardChanged) {
            loadBoard(boardId, false);
            for (const auto& l : store.lists(boardId))
                if (!store.cards(l.id).empty()) lists.insert(l.id);
        }
        for (const auto& id : removed) {
            auto cached = store.card(id);
            if (cached) lists.insert(cached->listId);
            removeCard(id, cached);
        }
        bool listGone = false;
        for (const auto& id : lists) {
            try {
                loadListCards(id, Priority::Visible);
            } catch (const ApiError& e) {
                if (e.status() != 404) throw;
                listGone = true;
            }
        }
        if (listGone) loadBoard(boardId, false);
        if (refreshOpen && !open.empty() && !removed.count(open)) openCard(open);
    }

    static constexpr size_t kActionLimit = 1000;  // what Trello returns per request at most

    void loadExtras(const std::string& cardId) {
        CardExtras loaded;
        try {
            loaded = api.cardExtras(cardId).get();
        } catch (const ApiError& e) {
            if (e.status() == 404) return;  // vanished between the two requests
            throw;
        }
        {
            std::lock_guard lk(mutex);
            extras[cardId] = std::move(loaded);
        }
        ChangeSet c;
        c.cardExtras = {cardId};
        sink.onChanged(c);
    }
};

SyncEngine::SyncEngine(TrelloApi& api, Store& store, Executor& executor, ChangeSink& sink,
                       SyncOptions options, EventSource* events)
    : impl_(std::make_shared<Impl>(api, store, executor, sink, std::move(options), events)) {}

SyncEngine::~SyncEngine() {
    if (impl_->events) {
        std::string board;
        {
            std::lock_guard lk(impl_->mutex);
            board = impl_->currentBoard;
        }
        if (!board.empty()) impl_->events->unwatch(board);
    }
    impl_->shutdown();
}

void SyncEngine::start() {
    impl_->post([p = impl_.get()] { p->loadBoards(); });
}

void SyncEngine::openBoard(const std::string& id) {
    std::string previous;
    {
        std::lock_guard lk(impl_->mutex);
        previous = impl_->currentBoard;
        impl_->currentBoard = id;
    }
    if (impl_->events && !previous.empty() && previous != id) impl_->events->unwatch(previous);
    impl_->post([p = impl_.get(), id] { p->loadBoard(id, true); });
}

void SyncEngine::openCard(const std::string& id) {
    impl_->post([p = impl_.get(), id] { p->openCard(id); });
}

void SyncEngine::expandList(const std::string& id) {
    impl_->post([p = impl_.get(), id] { p->expandList(id); });
}

std::vector<Board> SyncEngine::boards() { return impl_->store.boards(); }
std::vector<List> SyncEngine::lists(const std::string& boardId) { return impl_->store.lists(boardId); }
std::vector<Card> SyncEngine::cards(const std::string& listId) { return impl_->store.cards(listId); }
std::optional<Card> SyncEngine::card(const std::string& id) { return impl_->store.card(id); }
std::vector<Comment> SyncEngine::comments(const std::string& cardId) {
    return impl_->store.comments(cardId);
}
std::vector<Label> SyncEngine::labels(const std::string& boardId) { return impl_->store.labels(boardId); }
std::vector<Member> SyncEngine::members(const std::string& boardId) {
    return impl_->store.members(boardId);
}

std::optional<CardExtras> SyncEngine::extras(const std::string& cardId) {
    std::lock_guard lk(impl_->mutex);
    auto it = impl_->extras.find(cardId);
    if (it == impl_->extras.end()) return std::nullopt;
    return it->second;
}

}  // namespace cardinal
