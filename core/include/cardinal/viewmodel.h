#pragma once

// View-models: everything a UI needs, with no UI toolkit and no threads to think about.
//
// Contract for UI code:
//  * Call VM methods and read VM state only on the UI thread.
//  * Signals fire only on the UI thread, from inside UiDispatcher::post callbacks.
//  * State is immutable snapshots; no VM call blocks, touches disk or the network.
//  * VMs must be destroyed before the AppModel that created them.

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cardinal/event_source.h"
#include "cardinal/executor.h"
#include "cardinal/model.h"
#include "cardinal/sync.h"
#include "cardinal/trello_api.h"
#include "cardinal/write_queue.h"

namespace cardinal {

// Runs a callback on the UI thread (g_idle_add, QMetaObject::invokeMethod, Dispatcher, ...).
class UiDispatcher {
public:
    virtual ~UiDispatcher() = default;
    virtual void post(std::function<void()> fn) = 0;
};

// UI-thread-only signal. A slot disconnected while an emit is running is not called, even if it
// was already queued for that emit (a page that tears itself down must not be called afterwards).
template <class... A>
class Signal {
public:
    using Fn = std::function<void(A...)>;
    int connect(Fn fn) {
        auto slot = std::make_shared<Slot>();
        slot->id = next_++;
        slot->fn = std::move(fn);
        slots_.push_back(slot);
        return slot->id;
    }
    void disconnect(int id) {
        for (auto it = slots_.begin(); it != slots_.end(); ++it)
            if ((*it)->id == id) {
                (*it)->live = false;
                slots_.erase(it);
                return;
            }
    }
    void emit(A... args) {
        auto copy = slots_;  // handlers may connect/disconnect while running
        for (auto& slot : copy)
            if (slot->live) slot->fn(args...);
    }

private:
    struct Slot {
        int id = 0;
        Fn fn;
        bool live = true;
    };
    std::vector<std::shared_ptr<Slot>> slots_;
    int next_ = 1;
};

template <class T>
class Observable {
public:
    explicit Observable(T initial = {}) : value_(std::move(initial)) {}
    const T& get() const { return value_; }
    void set(T v) {
        value_ = std::move(v);
        changed.emit(value_);
    }
    Signal<const T&> changed;

private:
    T value_;
};

// ---- state ----

struct LabelChip {
    std::string id, name, color;
    bool operator==(const LabelChip&) const = default;
};
struct MemberChip {
    std::string id, fullName, initials;
    bool operator==(const MemberChip&) const = default;
};

struct CardRow {
    std::string id;
    std::string title;
    double pos = 0;
    std::vector<LabelChip> labels;
    std::vector<MemberChip> members;
    int commentCount = 0;
    bool operator==(const CardRow&) const = default;
};

struct ColumnState {
    std::string id;
    std::string name;
    double pos = 0;
    int cardCount = 0;   // as reported by Trello; equals cards.size() once loaded
    bool loaded = false;  // false: a cold list whose cards were not fetched yet
    std::vector<CardRow> cards;
    bool operator==(const ColumnState&) const = default;
};

struct AppStatus {
    bool offline = false;       // last network attempt failed at the transport level
    bool loginRequired = false;  // Trello rejected the token (401)
    int pendingWrites = 0;       // edits not yet confirmed by Trello
    std::string lastError;
    bool operator==(const AppStatus&) const = default;
};

struct BoardPickerState {
    std::vector<Board> boards;
    bool loading = true;
    bool operator==(const BoardPickerState&) const = default;
};

struct BoardHeader {
    std::string name;
    bool loading = true;
    bool operator==(const BoardHeader&) const = default;
};

struct CommentRow {
    std::string id;
    std::string authorName;
    std::string authorInitials;
    std::string text;
    std::string date;
    bool sending = false;  // optimistic, not confirmed yet
    bool operator==(const CommentRow&) const = default;
};

// Something the panel should tell the user. The UI turns codes into words.
struct PanelNotice {
    enum class Kind { RemovedRemotely, ConflictOverwritten, WriteRejected };
    Kind kind;
    std::string field;   // "name" / "desc" for conflicts; the operation kind for rejections
    std::string detail;  // their text for conflicts; the error for rejections
    bool operator==(const PanelNotice&) const = default;
};

struct CardPanelState {
    enum class Status { Loading, Ready, Removed };
    Status status = Status::Loading;
    std::string cardId;
    std::string listName;
    std::string title;
    std::string description;
    std::vector<LabelChip> labels;
    std::vector<MemberChip> members;
    std::vector<LabelChip> boardLabels;    // for the label picker
    std::vector<MemberChip> boardMembers;  // for the member picker
    std::vector<CommentRow> comments;      // newest first
    std::vector<Checklist> checklists;     // read-only in v1
    std::vector<Attachment> attachments;   // read-only in v1
    bool extrasLoaded = false;
    std::vector<PanelNotice> notices;
    bool operator==(const CardPanelState&) const = default;
};

// ---- view-models ----

class AppModel;
struct AppServices;
struct BoardPublication;

class BoardPickerVM {
public:
    Observable<BoardPickerState>& state() { return state_; }

private:
    friend class AppModel;
    Observable<BoardPickerState> state_;
};

class BoardVM {
public:
    ~BoardVM();
    const std::string& boardId() const { return boardId_; }
    Observable<BoardHeader>& header() { return header_; }

    std::vector<std::shared_ptr<const ColumnState>> columns() const { return columns_; }
    std::shared_ptr<const ColumnState> column(const std::string& listId) const;
    const std::vector<LabelChip>& labels() const { return labels_; }
    const std::vector<MemberChip>& members() const { return members_; }

    Signal<> structureChanged;                 // columns added/removed/renamed, labels, members
    Signal<const std::string&> columnChanged;  // one column's cards or count changed

    void expandList(const std::string& listId);
    // `index` is the position among the target column's cards, not counting the moved card.
    void moveCard(const std::string& cardId, const std::string& toListId, size_t index);
    void createCard(const std::string& listId, const std::string& title);

private:
    friend class AppModel;
    friend struct AppServices;
    struct Core;
    BoardVM(std::shared_ptr<AppServices> services, std::string boardId);
    void apply(BoardPublication&& built);

    std::shared_ptr<AppServices> services_;
    std::string boardId_;
    std::shared_ptr<Core> core_;
    Observable<BoardHeader> header_;
    std::vector<std::shared_ptr<const ColumnState>> columns_;
    std::vector<LabelChip> labels_;
    std::vector<MemberChip> members_;
};

class CardPanelVM {
public:
    ~CardPanelVM();
    Observable<CardPanelState>& state() { return state_; }

    void setTitle(const std::string& title);
    void setDescription(const std::string& description);
    void addLabel(const std::string& labelId);
    void addMember(const std::string& memberId);
    void addComment(const std::string& text);
    void dismissNotices();

private:
    friend class AppModel;
    friend struct AppServices;
    struct Core;
    CardPanelVM(std::shared_ptr<AppServices> services, std::string cardId);

    std::shared_ptr<AppServices> services_;
    std::shared_ptr<Core> core_;
    Observable<CardPanelState> state_;
};

// Everything a UI constructs. Owns the sync engine and the write queue.
class AppModel {
public:
    AppModel(TrelloApi& api, Store& store, Executor& executor, UiDispatcher& ui,
             EventSource* events = nullptr, SyncOptions sync = {}, WriteOptions write = {});
    ~AppModel();
    AppModel(const AppModel&) = delete;
    AppModel& operator=(const AppModel&) = delete;

    void start();  // refresh the board list
    std::shared_ptr<BoardPickerVM> boardPicker() { return picker_; }
    std::shared_ptr<BoardVM> openBoard(const std::string& boardId);
    std::shared_ptr<CardPanelVM> openCard(const std::string& cardId);

    Observable<AppStatus>& status() { return *status_; }
    void retry();           // try again now: pending writes and the board list
    void loginCompleted();  // a new token is in place

private:
    std::shared_ptr<AppServices> services_;
    std::shared_ptr<BoardPickerVM> picker_;
    std::shared_ptr<Observable<AppStatus>> status_;
};

}  // namespace cardinal
