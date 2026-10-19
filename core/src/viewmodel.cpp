#include "cardinal/viewmodel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <set>

namespace cardinal {
namespace {

constexpr double kPosStep = 65536;

bool isTemp(const std::string& id) { return id.rfind("tmp-", 0) == 0; }

template <class T>
bool has(const std::vector<T>& v, const T& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

}  // namespace

// Receives every cache change. Called on any thread; must not block.
struct Listener {
    virtual ~Listener() = default;
    virtual void changed(const ChangeSet&) {}
    virtual void conflict(const Conflict&) {}
    virtual void rejected(const Rejection&) {}
};

// Shared guts of AppModel and the VMs: executor/UI plumbing, change fan-out, app status.
struct AppServices : std::enable_shared_from_this<AppServices> {
    AppServices(TrelloApi& a, Store& s, Executor& e, UiDispatcher& u)
        : api(a), store(s), exec(e), ui(u) {}

    TrelloApi& api;
    Store& store;
    Executor& exec;
    UiDispatcher& ui;
    std::unique_ptr<SyncEngine> engine;
    std::unique_ptr<WriteQueue> queue;

    std::mutex mutex;
    std::condition_variable idle;
    std::atomic<bool> dead{false};
    int running = 0;

    std::string currentBoard;
    std::vector<std::weak_ptr<Listener>> listeners;
    std::weak_ptr<BoardPickerVM> picker;
    bool pickerLoading = true;
    bool pickerScheduled = false;

    AppStatus status;
    int statusSeq = 0;
    struct UiCell {  // touched on the UI thread only
        int applied = 0;
    };
    std::shared_ptr<UiCell> uiCell = std::make_shared<UiCell>();
    std::weak_ptr<Observable<AppStatus>> statusCell;

    // ---- plumbing ----

    void postWork(std::function<void()> fn) {
        exec.post([weak = weak_from_this(), fn = std::move(fn)] {
            auto self = weak.lock();
            if (!self) return;
            {
                std::lock_guard lk(self->mutex);
                if (self->dead) return;
                ++self->running;
            }
            try {
                fn();
            } catch (const std::exception& e) {
                self->reportError(0, e.what());
            }
            std::lock_guard lk(self->mutex);
            --self->running;
            self->idle.notify_all();
        });
    }

    void postUi(std::function<void()> fn) {
        ui.post([weak = weak_from_this(), fn = std::move(fn)] {
            auto self = weak.lock();
            if (!self || self->dead) return;
            fn();
        });
    }

    void shutdown() {
        {
            std::unique_lock lk(mutex);
            dead = true;
            idle.wait(lk, [this] { return running == 0; });
        }
        queue.reset();
        engine.reset();
    }

    // ---- fan-out ----

    void register_(std::weak_ptr<Listener> l) {
        std::lock_guard lk(mutex);
        listeners.push_back(std::move(l));
    }

    template <class F>
    void forEachListener(F f) {
        std::vector<std::shared_ptr<Listener>> live;
        {
            std::lock_guard lk(mutex);
            for (auto it = listeners.begin(); it != listeners.end();) {
                if (auto l = it->lock()) {
                    live.push_back(std::move(l));
                    ++it;
                } else {
                    it = listeners.erase(it);
                }
            }
        }
        for (auto& l : live) f(*l);
    }

    // ---- status ----

    void updateStatus(const std::function<void(AppStatus&)>& edit) {
        AppStatus snapshot;
        int seq;
        {
            std::lock_guard lk(mutex);
            if (queue) status.pendingWrites = static_cast<int>(queue->pendingCount());
            edit(status);
            snapshot = status;
            seq = ++statusSeq;
        }
        postUi([this, snapshot, seq] {
            if (seq <= uiCell->applied) return;  // a newer one already got through
            uiCell->applied = seq;
            if (auto cell = statusCell.lock())
                if (!(cell->get() == snapshot)) cell->set(snapshot);
        });
    }

    // Trello answered something: we are online. Coming back from offline means whatever was
    // waiting (unsent writes, a board that failed to load) should go now, not on the next edit.
    void networkOk() {
        bool recovered = false;
        updateStatus([&](AppStatus& s) {
            recovered = s.offline;
            s.offline = false;
        });
        if (recovered && !dead) {
            postWork([this] {
                if (queue) queue->flush();
                if (engine) engine->refresh();
            });
        }
    }

    void reportError(int code, const std::string& what) {
        updateStatus([&](AppStatus& s) {
            s.lastError = what;
            if (code == 401) s.loginRequired = true;
            if (code == 0) s.offline = true;
        });
        bool republish = false;
        {
            std::lock_guard lk(mutex);
            republish = pickerLoading;
            pickerLoading = false;
        }
        if (republish) schedulePicker();
    }

    // ---- board picker ----

    void schedulePicker() {
        {
            std::lock_guard lk(mutex);
            if (pickerScheduled) return;
            pickerScheduled = true;
        }
        postWork([this] {
            bool loading;
            {
                std::lock_guard lk(mutex);
                pickerScheduled = false;
                loading = pickerLoading;
            }
            if (!engine) return;
            BoardPickerState state{engine->boards(), loading};
            postUi([this, state = std::move(state)]() mutable {
                if (auto p = picker.lock())
                    if (!(p->state().get() == state)) p->state().set(std::move(state));
            });
        });
    }

    // ---- sinks ----

    struct EngineSink : ChangeSink {
        explicit EngineSink(AppServices& s) : svc(s) {}
        void onChanged(const ChangeSet& cs) override {
            if (svc.dead) return;
            svc.networkOk();
            if (cs.boards) {
                {
                    std::lock_guard lk(svc.mutex);
                    svc.pickerLoading = false;
                }
                svc.schedulePicker();
            }
            svc.forEachListener([&](Listener& l) { l.changed(cs); });
        }
        void onError(const SyncError& e) override {
            if (!svc.dead) svc.reportError(e.status, e.what);
        }
        AppServices& svc;
    };

    struct QueueSink : WriteSink {
        explicit QueueSink(AppServices& s) : svc(s) {}
        void onChanged(const ChangeSet& cs) override {
            if (svc.dead) return;
            svc.forEachListener([&](Listener& l) { l.changed(cs); });
        }
        void onError(const SyncError& e) override {
            if (!svc.dead) svc.reportError(e.status, e.what);
        }
        void onRejected(const Rejection& r) override {
            if (svc.dead) return;
            svc.updateStatus([&](AppStatus& s) { s.lastError = r.what; });
            svc.forEachListener([&](Listener& l) { l.rejected(r); });
        }
        void onConflict(const Conflict& c) override {
            if (svc.dead) return;
            svc.forEachListener([&](Listener& l) { l.conflict(c); });
        }
        void onPendingChanged() override {
            if (!svc.dead) svc.updateStatus([](AppStatus&) {});
        }
        void onNetworkOk() override {
            if (!svc.dead) svc.networkOk();
        }
        AppServices& svc;
    };

    EngineSink engineSink{*this};
    QueueSink queueSink{*this};

    void enqueue(Op op) {
        postWork([this, op = std::move(op)] {
            if (!queue) return;
            try {
                queue->enqueue(op);
            } catch (const std::exception& e) {
                reportError(-1, e.what());  // -1: a plain message, not a connectivity problem
            }
        });
    }
};

// ---- BoardVM ----

struct BoardPublication {
    BoardHeader header;
    bool structure = false;
    std::vector<std::shared_ptr<const ColumnState>> columns;
    std::vector<std::string> changedColumns;
    std::vector<LabelChip> labels;
    std::vector<MemberChip> members;
};

struct BoardVM::Core : Listener, std::enable_shared_from_this<Core> {
    Core(std::shared_ptr<AppServices> s, std::string id) : svc(std::move(s)), boardId(std::move(id)) {}

    std::shared_ptr<AppServices> svc;
    std::string boardId;
    std::weak_ptr<BoardVM> owner;

    std::mutex m;  // dirty state
    bool scheduled = false;
    bool snapshotSeen = false;
    std::set<std::string> dirtyLists, dirtyCards;

    std::mutex buildMu;  // worker-side memory of what was published
    std::map<std::string, std::shared_ptr<const ColumnState>> last;
    std::vector<std::string> lastOrder;
    std::vector<LabelChip> lastLabels;
    std::vector<MemberChip> lastMembers;
    std::string lastName;
    BoardHeader lastHeader;
    bool published = false;

    void changed(const ChangeSet& cs) override {
        {
            std::lock_guard lk(m);
            if (has(cs.boardLists, boardId)) snapshotSeen = true;
            for (const auto& l : cs.listCards) dirtyLists.insert(l);
            for (const auto& c : cs.cardDetails) dirtyCards.insert(c);
        }
        schedule();
    }

    void schedule() {
        {
            std::lock_guard lk(m);
            if (scheduled) return;
            scheduled = true;
        }
        svc->postWork([weak = weak_from_this()] {
            if (auto self = weak.lock()) self->rebuild();
        });
    }

    static bool sameColumn(const ColumnState& a, const ColumnState& b) { return a == b; }

    void rebuild() {
        std::set<std::string> lists, cards;
        bool seen;
        {
            std::lock_guard lk(m);
            lists.swap(dirtyLists);
            cards.swap(dirtyCards);
            scheduled = false;
            seen = snapshotSeen;
        }
        if (!svc->engine) return;
        SyncEngine& engine = *svc->engine;

        std::lock_guard build(buildMu);
        for (const auto& id : cards)
            if (auto c = engine.card(id)) lists.insert(c->listId);

        const auto rawLists = engine.lists(boardId);
        std::string name;
        for (const auto& b : engine.boards())
            if (b.id == boardId) name = b.name;

        std::map<std::string, LabelChip> labelById;
        std::map<std::string, MemberChip> memberById;
        std::vector<LabelChip> labels;
        std::vector<MemberChip> members;
        for (const auto& l : engine.labels(boardId)) {
            labels.push_back({l.id, l.name, l.color});
            labelById[l.id] = labels.back();
        }
        for (const auto& m2 : engine.members(boardId)) {
            members.push_back({m2.id, m2.fullName, m2.initials});
            memberById[m2.id] = members.back();
        }

        BoardPublication out;
        std::vector<std::string> order;
        std::map<std::string, std::shared_ptr<const ColumnState>> now;
        for (const auto& l : rawLists) {
            order.push_back(l.id);
            auto prev = last.find(l.id);
            bool need = prev == last.end() || lists.count(l.id) || prev->second->name != l.name ||
                        prev->second->pos != l.pos;
            if (!need && !prev->second->loaded && prev->second->cardCount != l.cardCount) need = true;
            if (!need) {
                now[l.id] = prev->second;
                continue;
            }
            auto col = std::make_shared<ColumnState>();
            col->id = l.id;
            col->name = l.name;
            col->pos = l.pos;
            auto cached = engine.cards(l.id);
            col->loaded = l.loaded || !cached.empty() || l.cardCount == 0;
            col->cardCount = col->loaded ? static_cast<int>(cached.size()) : l.cardCount;
            for (const auto& c : cached) {
                CardRow row{c.id, c.name, c.pos, {}, {}, c.commentCount};
                for (const auto& id : c.labelIds) {
                    auto it = labelById.find(id);
                    if (it != labelById.end()) row.labels.push_back(it->second);
                }
                for (const auto& id : c.memberIds) {
                    auto it = memberById.find(id);
                    if (it != memberById.end()) row.members.push_back(it->second);
                }
                col->cards.push_back(std::move(row));
            }
            if (prev != last.end() && sameColumn(*prev->second, *col)) {
                now[l.id] = prev->second;  // nothing visible changed: keep the old snapshot
            } else {
                out.changedColumns.push_back(l.id);
                now[l.id] = col;
            }
        }
        for (const auto& id : order) out.columns.push_back(now[id]);

        out.structure = !published || order != lastOrder || labels != lastLabels ||
                        members != lastMembers || name != lastName;
        if (!out.structure) {  // a renamed or moved column also counts
            for (const auto& id : order)
                if (!last.count(id) || last[id]->name != now[id]->name || last[id]->pos != now[id]->pos)
                    out.structure = true;
        }
        out.labels = labels;
        out.members = members;
        out.header = {name, rawLists.empty() && !seen};

        last = now;
        lastOrder = order;
        lastLabels = labels;
        lastMembers = members;
        lastName = name;
        bool first = !published;
        published = true;
        bool headerChanged = !(out.header == lastHeader);
        lastHeader = out.header;
        if (!first && !out.structure && out.changedColumns.empty() && !headerChanged) return;

        svc->postUi([weak = owner, out = std::move(out)]() mutable {
            if (auto vm = weak.lock()) vm->apply(std::move(out));
        });
    }
};

BoardVM::BoardVM(std::shared_ptr<AppServices> services, std::string boardId)
    : services_(std::move(services)),
      boardId_(std::move(boardId)),
      core_(std::make_shared<Core>(services_, boardId_)) {}

BoardVM::~BoardVM() = default;

std::shared_ptr<const ColumnState> BoardVM::column(const std::string& listId) const {
    for (const auto& c : columns_)
        if (c->id == listId) return c;
    return nullptr;
}

void BoardVM::apply(BoardPublication&& built) {
    columns_ = std::move(built.columns);
    labels_ = std::move(built.labels);
    members_ = std::move(built.members);
    if (!(header_.get() == built.header)) header_.set(built.header);
    if (built.structure) structureChanged.emit();
    for (const auto& id : built.changedColumns) columnChanged.emit(id);
}

void BoardVM::expandList(const std::string& listId) {
    if (services_->engine && !services_->dead) services_->engine->expandList(listId);
}

void BoardVM::moveCard(const std::string& cardId, const std::string& toListId, size_t index) {
    double pos = kPosStep;
    if (auto target = column(toListId)) {
        std::vector<double> others;
        for (const auto& c : target->cards)
            if (c.id != cardId) others.push_back(c.pos);
        index = std::min(index, others.size());
        if (others.empty())
            pos = kPosStep;
        else if (index == 0)
            pos = others.front() / 2;
        else if (index == others.size())
            pos = others.back() + kPosStep;
        else
            pos = (others[index - 1] + others[index]) / 2;
    }
    services_->enqueue(MoveCard{cardId, toListId, pos});
}

void BoardVM::createCard(const std::string& listId, const std::string& title) {
    services_->enqueue(CreateCard{listId, title});
}

// ---- CardPanelVM ----

struct CardPanelVM::Core : Listener, std::enable_shared_from_this<Core> {
    Core(std::shared_ptr<AppServices> s, std::string id, std::string board)
        : svc(std::move(s)), boardId(std::move(board)), cardId(std::move(id)) {}

    std::shared_ptr<AppServices> svc;
    std::string boardId;
    std::weak_ptr<CardPanelVM> owner;

    std::mutex m;
    std::string cardId;
    bool removed = false;
    bool scheduled = false;
    std::vector<PanelNotice> notices;

    std::mutex buildMu;
    CardPanelState lastState;

    std::string id() {
        std::lock_guard lk(m);
        return cardId;
    }

    void changed(const ChangeSet& cs) override {
        std::string openNow;
        bool relevant = false;
        bool renamed = false;
        std::string newId;
        {
            std::lock_guard lk(m);
            for (const auto& [from, to] : cs.renamedCards)
                if (from == cardId) {
                    cardId = to;
                    newId = to;
                    renamed = true;
                    relevant = true;
                }
            if (has(cs.removedCards, cardId)) {
                removed = true;
                relevant = true;
            }
            if (has(cs.cardDetails, cardId) || has(cs.cardExtras, cardId)) relevant = true;
            // Row-level edits, and label/member renames, can change what the panel shows.
            if (!cs.listCards.empty() || !cs.boardLists.empty()) relevant = true;
        }
        if (renamed && svc->engine) svc->engine->openCard(newId);
        if (relevant) schedule();
    }

    void conflict(const Conflict& c) override {
        {
            std::lock_guard lk(m);
            if (c.cardId != cardId) return;
            notices.push_back({PanelNotice::Kind::ConflictOverwritten, c.field, c.theirs});
        }
        schedule();
    }

    void rejected(const Rejection& r) override {
        {
            std::lock_guard lk(m);
            if (r.cardId != cardId) return;
            notices.push_back({PanelNotice::Kind::WriteRejected, r.kind, r.what});
        }
        schedule();
    }

    void dismiss() {
        {
            std::lock_guard lk(m);
            notices.clear();
        }
        schedule();
    }

    void schedule() {
        {
            std::lock_guard lk(m);
            if (scheduled) return;
            scheduled = true;
        }
        svc->postWork([weak = weak_from_this()] {
            if (auto self = weak.lock()) self->rebuild();
        });
    }

    void rebuild() {
        std::string id;
        bool wasRemoved;
        std::vector<PanelNotice> noticesNow;
        {
            std::lock_guard lk(m);
            scheduled = false;
            id = cardId;
            wasRemoved = removed;
            noticesNow = notices;
        }
        if (!svc->engine) return;
        SyncEngine& engine = *svc->engine;
        std::lock_guard build(buildMu);

        CardPanelState s;
        if (wasRemoved) {
            s = lastState;  // keep showing what the user was looking at
            s.status = CardPanelState::Status::Removed;
            if (!has(noticesNow, PanelNotice{PanelNotice::Kind::RemovedRemotely, "", ""}))
                noticesNow.insert(noticesNow.begin(), {PanelNotice::Kind::RemovedRemotely, "", ""});
            s.notices = noticesNow;
        } else if (auto card = engine.card(id)) {
            s.status = CardPanelState::Status::Ready;
            s.cardId = id;
            s.title = card->name;
            s.description = card->desc;

            std::map<std::string, LabelChip> labelById;
            std::map<std::string, MemberChip> memberById;
            for (const auto& l : engine.labels(boardId)) {
                s.boardLabels.push_back({l.id, l.name, l.color});
                labelById[l.id] = s.boardLabels.back();
            }
            for (const auto& mm : engine.members(boardId)) {
                s.boardMembers.push_back({mm.id, mm.fullName, mm.initials});
                memberById[mm.id] = s.boardMembers.back();
            }
            for (const auto& l : engine.lists(boardId))
                if (l.id == card->listId) s.listName = l.name;
            for (const auto& lid : card->labelIds)
                if (labelById.count(lid)) s.labels.push_back(labelById[lid]);
            for (const auto& mid : card->memberIds)
                if (memberById.count(mid)) s.members.push_back(memberById[mid]);
            for (const auto& c : engine.comments(id)) {
                CommentRow row{c.id, "", "", c.text, c.date, isTemp(c.id)};
                auto it = memberById.find(c.authorId);
                if (it != memberById.end()) {
                    row.authorName = it->second.fullName;
                    row.authorInitials = it->second.initials;
                }
                s.comments.push_back(std::move(row));
            }
            if (auto extras = engine.extras(id)) {
                s.extrasLoaded = true;
                s.checklists = extras->checklists;
                s.attachments = extras->attachments;
            }
            s.notices = noticesNow;
        } else {
            s.status = CardPanelState::Status::Loading;
            s.cardId = id;
            s.notices = noticesNow;
        }
        if (s == lastState) return;
        lastState = s;
        svc->postUi([weak = owner, s = std::move(s)]() mutable {
            if (auto vm = weak.lock()) vm->state_.set(std::move(s));
        });
    }
};

CardPanelVM::CardPanelVM(std::shared_ptr<AppServices> services, std::string cardId) : services_(services) {
    std::string board;
    {
        std::lock_guard lk(services_->mutex);
        board = services_->currentBoard;
    }
    core_ = std::make_shared<Core>(services_, std::move(cardId), std::move(board));
}

CardPanelVM::~CardPanelVM() = default;

void CardPanelVM::setTitle(const std::string& title) {
    services_->enqueue(EditCard{core_->id(), title, std::nullopt});
}
void CardPanelVM::setDescription(const std::string& description) {
    services_->enqueue(EditCard{core_->id(), std::nullopt, description});
}
void CardPanelVM::addLabel(const std::string& labelId) {
    services_->enqueue(AddLabel{core_->id(), labelId});
}
void CardPanelVM::addMember(const std::string& memberId) {
    services_->enqueue(AddMember{core_->id(), memberId});
}
void CardPanelVM::addComment(const std::string& text) {
    services_->enqueue(AddComment{core_->id(), text});
}
void CardPanelVM::dismissNotices() { core_->dismiss(); }

// ---- AppModel ----

AppModel::AppModel(TrelloApi& api, Store& store, Executor& executor, UiDispatcher& ui,
                   EventSource* events, SyncOptions sync, WriteOptions write)
    : services_(std::make_shared<AppServices>(api, store, executor, ui)),
      picker_(std::shared_ptr<BoardPickerVM>(new BoardPickerVM())),
      status_(std::make_shared<Observable<AppStatus>>()) {
    services_->picker = picker_;
    services_->statusCell = status_;
    services_->engine = std::make_unique<SyncEngine>(api, store, executor, services_->engineSink,
                                                     std::move(sync), events);
    services_->queue = std::make_unique<WriteQueue>(api, store, executor, services_->queueSink,
                                                    std::move(write));
}

AppModel::~AppModel() { services_->shutdown(); }

void AppModel::start() {
    services_->schedulePicker();  // cache first
    services_->engine->start();
}

std::shared_ptr<BoardVM> AppModel::openBoard(const std::string& boardId) {
    {
        std::lock_guard lk(services_->mutex);
        services_->currentBoard = boardId;
    }
    auto vm = std::shared_ptr<BoardVM>(new BoardVM(services_, boardId));
    vm->core_->owner = vm;
    services_->register_(vm->core_);
    vm->core_->schedule();  // cache first
    services_->engine->openBoard(boardId);
    return vm;
}

std::shared_ptr<CardPanelVM> AppModel::openCard(const std::string& cardId) {
    auto vm = std::shared_ptr<CardPanelVM>(new CardPanelVM(services_, cardId));
    vm->core_->owner = vm;
    services_->register_(vm->core_);
    vm->core_->schedule();  // cache first
    if (!isTemp(cardId)) services_->engine->openCard(cardId);
    return vm;
}

void AppModel::retry() {
    services_->queue->flush();
    services_->engine->start();
}

void AppModel::onNetworkOk() { services_->networkOk(); }

void AppModel::onNetworkError(const SyncError& error) { services_->reportError(error.status, error.what); }

void AppModel::loginCompleted() {
    services_->updateStatus([](AppStatus& s) { s.loginRequired = false; });
    retry();
}

}  // namespace cardinal
