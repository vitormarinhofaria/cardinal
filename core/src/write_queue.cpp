#include "cardinal/write_queue.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>

#include "cardinal/time_util.h"

namespace cardinal {
namespace {

using nlohmann::json;

constexpr double kPosStep = 65536;  // Trello's own spacing between card positions

std::vector<std::string> ids(const json& j) {
    return j.is_array() ? j.get<std::vector<std::string>>() : std::vector<std::string>{};
}

template <class T>
void addOnce(std::vector<T>& v, const T& x) {
    if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

enum class Failure { Transient, Gone, Permanent };

struct Error {
    Failure kind;
    int status = 0;
    std::string what;
};

Error classify(const std::exception_ptr& ep) {
    try {
        std::rethrow_exception(ep);
    } catch (const ApiError& e) {
        int s = e.status();
        if (s == 401 || s == 408 || s == 429 || s >= 500) return {Failure::Transient, s, e.what()};
        if (s == 404) return {Failure::Gone, s, e.what()};
        return {Failure::Permanent, s, e.what() + std::string(": ") + e.body()};
    } catch (const std::exception& e) {
        return {Failure::Transient, 0, e.what()};
    }
}

}  // namespace

struct WriteQueue::Impl : std::enable_shared_from_this<Impl> {
    Impl(TrelloApi& a, Store& s, Executor& e, WriteSink& k, WriteOptions o)
        : api(a), store(s), exec(e), sink(k), opts(std::move(o)), rng(std::random_device{}()) {}

    TrelloApi& api;
    Store& store;
    Executor& exec;
    WriteSink& sink;
    const WriteOptions opts;

    std::mutex mutex;  // pending, processing, closed, running
    std::condition_variable idle;
    std::deque<PendingOp> pending;
    bool processing = false;
    bool blocked = false;         // a transient failure paused sending until the next retry request
    bool retryRequested = false;  // a retry was requested while an attempt was running
    bool closed = false;
    int running = 0;

    std::mutex cacheMu;  // serializes read-modify-write of cache rows (taken before `mutex`)
    std::mutex rngMu;
    std::mt19937_64 rng;

    std::string tempId() {
        std::lock_guard lk(rngMu);
        static constexpr char kHex[] = "0123456789abcdef";
        std::string s = "tmp-";
        for (int i = 0; i < 12; ++i) s += kHex[rng() & 15];
        return s;
    }

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

    // ---- optimistic changes (caller holds cacheMu) ----

    Card requireCard(const std::string& id) {
        auto c = store.card(id);
        if (!c) throw std::invalid_argument("unknown card " + id);
        return *c;
    }

    void putCard(const Card& c) {
        std::vector<Card> one{c};
        store.upsertCards(one);
    }

    // Builds payload (reading the cache) without touching it. Returns {kind, cardId, payload}.
    struct Built {
        std::string kind;
        std::string cardId;
        json payload;
    };

    Built build(const MoveCard& o) {
        Card c = requireCard(o.cardId);
        return {"move", o.cardId,
                {{"cardId", o.cardId}, {"toListId", o.toListId}, {"pos", o.pos},
                 {"before", {{"listId", c.listId}, {"pos", c.pos}}}}};
    }
    Built build(const CreateCard& o) {
        double pos = kPosStep;
        for (const auto& c : store.cards(o.listId)) pos = std::max(pos, c.pos + kPosStep);
        std::string id = tempId();
        return {"create", id, {{"tempId", id}, {"listId", o.listId}, {"name", o.name}, {"pos", pos}}};
    }
    Built build(const EditCard& o) {
        Card c = requireCard(o.cardId);
        json p = {{"cardId", o.cardId}, {"baseActivity", c.dateLastActivity}, {"before", json::object()}};
        if (o.name) {
            p["name"] = *o.name;
            p["before"]["name"] = c.name;
        }
        if (o.desc) {
            p["desc"] = *o.desc;
            p["before"]["desc"] = c.desc;
        }
        return {"edit", o.cardId, p};
    }
    Built build(const AddLabel& o) {
        Card c = requireCard(o.cardId);
        return {"label", o.cardId,
                {{"cardId", o.cardId}, {"labelId", o.labelId}, {"before", {{"labelIds", c.labelIds}}}}};
    }
    Built build(const AddMember& o) {
        Card c = requireCard(o.cardId);
        return {"member", o.cardId,
                {{"cardId", o.cardId}, {"memberId", o.memberId}, {"before", {{"memberIds", c.memberIds}}}}};
    }
    Built build(const AddComment& o) {
        requireCard(o.cardId);
        return {"comment", o.cardId,
                {{"cardId", o.cardId}, {"text", o.text}, {"tempCommentId", tempId()},
                 {"author", opts.selfMemberId}, {"date", isoUtc(opts.now())}}};
    }

    ChangeSet apply(const std::string& kind, const json& p) {
        ChangeSet change;
        if (kind == "move") {
            Card c = requireCard(p["cardId"]);
            change.listCards = {p["toListId"], c.listId};
            c.listId = p["toListId"];
            c.pos = p["pos"];
            putCard(c);
        } else if (kind == "create") {
            Card c;
            c.id = p["tempId"];
            c.listId = p["listId"];
            c.name = p["name"];
            c.pos = p["pos"];
            putCard(c);
            change.listCards = {c.listId};
        } else if (kind == "edit") {
            Card c = requireCard(p["cardId"]);
            if (p.contains("name")) c.name = p["name"];
            if (p.contains("desc")) c.desc = p["desc"];
            putCard(c);
            change.cardDetails = {c.id};
            change.listCards = {c.listId};
        } else if (kind == "label" || kind == "member") {
            Card c = requireCard(p["cardId"]);
            if (kind == "label")
                addOnce<std::string>(c.labelIds, p["labelId"]);
            else
                addOnce<std::string>(c.memberIds, p["memberId"]);
            putCard(c);
            change.cardDetails = {c.id};
            change.listCards = {c.listId};
        } else if (kind == "comment") {
            Card c = requireCard(p["cardId"]);
            auto comments = store.comments(c.id);
            comments.insert(comments.begin(), {p["tempCommentId"], p["author"], p["text"], p["date"]});
            store.replaceComments(c.id, comments);
            c.commentCount += 1;
            putCard(c);
            change.cardDetails = {c.id};
            change.listCards = {c.listId};
        }
        return change;
    }

    ChangeSet revert(const std::string& kind, const json& p) {
        ChangeSet change;
        if (kind == "create") {
            std::string id = p["tempId"];
            store.deleteCard(id);
            change.listCards = {p["listId"]};
            change.removedCards = {id};
            return change;
        }
        auto cached = store.card(p["cardId"]);
        if (!cached) return change;
        Card c = *cached;
        change.listCards = {c.listId};
        if (kind == "move") {
            c.listId = p["before"]["listId"];
            c.pos = p["before"]["pos"];
            change.listCards.push_back(c.listId);
        } else if (kind == "edit") {
            if (p["before"].contains("name")) c.name = p["before"]["name"];
            if (p["before"].contains("desc")) c.desc = p["before"]["desc"];
            change.cardDetails = {c.id};
        } else if (kind == "label") {
            c.labelIds = ids(p["before"]["labelIds"]);
            change.cardDetails = {c.id};
        } else if (kind == "member") {
            c.memberIds = ids(p["before"]["memberIds"]);
            change.cardDetails = {c.id};
        } else if (kind == "comment") {
            auto comments = store.comments(c.id);
            std::string temp = p["tempCommentId"];
            comments.erase(std::remove_if(comments.begin(), comments.end(),
                                          [&](const Comment& m) { return m.id == temp; }),
                           comments.end());
            store.replaceComments(c.id, comments);
            c.commentCount = std::max(0, c.commentCount - 1);
            change.cardDetails = {c.id};
        }
        putCard(c);
        return change;
    }

    // ---- enqueue / process ----

    Enqueued enqueue(const Op& op) {
        std::lock_guard cache(cacheMu);
        Built b = std::visit([this](const auto& o) { return build(o); }, op);
        std::int64_t id = store.addPendingOp(b.kind, b.cardId, b.payload.dump(), opts.now());
        ChangeSet change = apply(b.kind, b.payload);
        {
            std::lock_guard lk(mutex);
            pending.push_back({id, b.kind, b.cardId, b.payload.dump(), opts.now()});
        }
        sink.onChanged(change);
        sink.onPendingChanged();
        retry();
        return {id, b.cardId};
    }

    // A new write or an explicit flush is a fresh chance to send; background jobs are not.
    void retry() {
        {
            std::lock_guard lk(mutex);
            blocked = false;
            retryRequested = true;
        }
        post([this] { process(); });
    }

    void process() {
        {
            std::lock_guard lk(mutex);
            if (processing || blocked) return;
            processing = true;
            retryRequested = false;
        }
        while (true) {
            PendingOp op;
            {
                std::lock_guard lk(mutex);
                if (pending.empty() || closed) {
                    processing = false;
                    return;
                }
                op = pending.front();
            }
            std::optional<Error> failure;
            try {
                send(op);
            } catch (...) {
                failure = classify(std::current_exception());
            }
            if (failure && failure->kind == Failure::Transient) {
                {
                    std::lock_guard lk(mutex);
                    processing = false;
                    blocked = !retryRequested;
                }
                sink.onError({failure->status, failure->what});
                return;
            }
            if (failure) reject(op, *failure);
            store.removePendingOp(op.id);
            {
                std::lock_guard lk(mutex);
                pending.pop_front();
            }
            sink.onPendingChanged();
        }
    }

    void reject(const PendingOp& op, const Error& e) {
        json p = json::parse(op.payload);
        ChangeSet change;
        {
            std::lock_guard cache(cacheMu);
            if (e.kind == Failure::Gone && op.kind != "create") {
                auto cached = store.card(op.cardId);
                store.deleteCard(op.cardId);
                change.removedCards = {op.cardId};
                if (cached) change.listCards = {cached->listId};
            } else {
                change = revert(op.kind, p);
            }
        }
        sink.onChanged(change);
        sink.onRejected({op.id, op.kind, op.cardId, e.status, e.what});
    }

    void send(const PendingOp& op) {
        const json p = json::parse(op.payload);
        const std::string& kind = op.kind;
        if (kind == "move") {
            Card done = api.moveCard(p["cardId"], p["toListId"], p["pos"]).get();
            confirmActivity(op.cardId, done.dateLastActivity);
        } else if (kind == "create") {
            Card real = api.createCard(p["listId"], p["name"]).get();
            confirmCreate(p["tempId"], real);
        } else if (kind == "edit") {
            checkConflict(p);
            CardEdit edit;
            if (p.contains("name")) edit.name = p["name"].get<std::string>();
            if (p.contains("desc")) edit.desc = p["desc"].get<std::string>();
            Card done = api.updateCard(p["cardId"], edit).get();
            confirmActivity(op.cardId, done.dateLastActivity);
        } else if (kind == "label") {
            api.addLabel(p["cardId"], p["labelId"]).get();
        } else if (kind == "member") {
            api.addMember(p["cardId"], p["memberId"]).get();
        } else if (kind == "comment") {
            Comment real = api.addComment(p["cardId"], p["text"]).get();
            confirmComment(op.cardId, p["tempCommentId"], real);
        }
    }

    void checkConflict(const json& p) {
        CardFields remote = api.cardFields(p["cardId"]).get();
        if (remote.dateLastActivity == p.value("baseActivity", std::string())) return;
        for (const char* field : {"name", "desc"}) {
            if (!p.contains(field)) continue;
            const std::string& theirs = std::string(field) == "name" ? remote.name : remote.desc;
            std::string before = p["before"].value(field, std::string());
            std::string mine = p[field];
            if (theirs != before && theirs != mine)
                sink.onConflict({p["cardId"], field, theirs, mine});
        }
    }

    void confirmActivity(const std::string& cardId, const std::string& activity) {
        std::lock_guard cache(cacheMu);
        auto c = store.card(cardId);
        if (!c || activity.empty()) return;
        c->dateLastActivity = activity;
        putCard(*c);
    }

    void confirmComment(const std::string& cardId, const std::string& tempId, const Comment& real) {
        {
            std::lock_guard cache(cacheMu);
            auto comments = store.comments(cardId);
            for (auto& c : comments)
                if (c.id == tempId) c = real;
            store.replaceComments(cardId, comments);
        }
        ChangeSet change;
        change.cardDetails = {cardId};
        sink.onChanged(change);
    }

    void confirmCreate(const std::string& tempId, Card real) {
        ChangeSet change;
        {
            std::lock_guard cache(cacheMu);
            if (auto temp = store.card(tempId)) {  // carry over edits made while it was temporary
                real.name = temp->name;
                real.desc = temp->desc;
                real.labelIds = temp->labelIds;
                real.memberIds = temp->memberIds;
                real.commentCount = temp->commentCount;
            }
            auto comments = store.comments(tempId);
            store.deleteCard(tempId);
            putCard(real);
            if (!comments.empty()) store.replaceComments(real.id, comments);
        }
        {
            std::lock_guard lk(mutex);  // later ops that still say "tmp-..." now mean the real card
            for (size_t i = 1; i < pending.size(); ++i) {
                auto& op = pending[i];
                if (op.cardId != tempId) continue;
                json p = json::parse(op.payload);
                p["cardId"] = real.id;
                op.cardId = real.id;
                op.payload = p.dump();
                store.updatePendingOp(op.id, op.cardId, op.payload);
            }
        }
        change.listCards = {real.listId};
        change.renamedCards = {{tempId, real.id}};
        sink.onChanged(change);
    }
};

WriteQueue::WriteQueue(TrelloApi& api, Store& store, Executor& executor, WriteSink& sink,
                       WriteOptions options)
    : impl_(std::make_shared<Impl>(api, store, executor, sink, std::move(options))) {
    for (auto& op : store.pendingOps()) impl_->pending.push_back(std::move(op));
    if (!impl_->pending.empty()) impl_->post([p = impl_.get()] { p->process(); });
}

WriteQueue::~WriteQueue() { impl_->shutdown(); }

Enqueued WriteQueue::enqueue(const Op& op) { return impl_->enqueue(op); }

void WriteQueue::flush() { impl_->retry(); }

size_t WriteQueue::pendingCount() {
    std::lock_guard lk(impl_->mutex);
    return impl_->pending.size();
}

}  // namespace cardinal
