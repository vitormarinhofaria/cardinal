#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "cardinal/executor.h"
#include "cardinal/store.h"
#include "cardinal/sync.h"
#include "cardinal/trello_api.h"

namespace cardinal {

struct MoveCard {
    std::string cardId;
    std::string toListId;
    double pos = 0;
};
struct CreateCard {
    std::string listId;
    std::string name;
};
struct EditCard {
    std::string cardId;
    std::optional<std::string> name;
    std::optional<std::string> desc;
};
struct AddLabel {
    std::string cardId;
    std::string labelId;
};
struct AddMember {
    std::string cardId;
    std::string memberId;
};
struct AddComment {
    std::string cardId;
    std::string text;
};
using Op = std::variant<MoveCard, CreateCard, EditCard, AddLabel, AddMember, AddComment>;

struct Enqueued {
    std::int64_t id = 0;
    std::string cardId;  // the affected card; a temporary "tmp-..." id for CreateCard
};

// Trello refused a write for good: the optimistic change was reverted.
struct Rejection {
    std::int64_t opId = 0;
    std::string kind;
    std::string cardId;
    int status = 0;
    std::string what;
};

// Someone else changed the field being edited. The edit is still written (last write wins).
struct Conflict {
    std::string cardId;
    std::string field;  // "name" or "desc"
    std::string theirs;
    std::string yours;
};

class WriteSink : public ChangeSink {
public:
    virtual void onRejected(const Rejection& rejection) = 0;
    virtual void onConflict(const Conflict& conflict) = 0;
};

struct WriteOptions {
    std::string selfMemberId;  // author shown on optimistic comments
    std::function<std::time_t()> now = [] { return std::time(nullptr); };
};

// Applies the user's writes to the cache at once and sends them to Trello in order, in the
// background. Writes survive restarts and offline periods. Sinks may be called concurrently.
class WriteQueue {
public:
    WriteQueue(TrelloApi& api, Store& store, Executor& executor, WriteSink& sink,
               WriteOptions options = {});
    ~WriteQueue();  // waits for the running job; queued jobs become no-ops
    WriteQueue(const WriteQueue&) = delete;
    WriteQueue& operator=(const WriteQueue&) = delete;

    // Throws std::invalid_argument for an unknown card.
    Enqueued enqueue(const Op& op);

    // Try to send what is pending, e.g. after the network came back or the user logged in again.
    void flush();

    size_t pendingCount();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace cardinal
