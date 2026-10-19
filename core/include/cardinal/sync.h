#pragma once

#include <chrono>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cardinal/event_source.h"
#include "cardinal/executor.h"
#include "cardinal/model.h"
#include "cardinal/store.h"
#include "cardinal/trello_api.h"

namespace cardinal {

// What changed in the cache. ChangeSink handlers run on executor threads, never on the UI thread,
// and may be called concurrently: implementations must be thread-safe.
struct ChangeSet {
    bool boards = false;
    std::vector<std::string> boardLists;    // boards whose lists, labels or members changed
    std::vector<std::string> listCards;     // lists whose cards changed
    std::vector<std::string> cardDetails;   // cards whose description or comments changed
    std::vector<std::string> cardExtras;    // cards whose checklists/attachments/members loaded
    std::vector<std::string> removedCards;  // cards deleted or archived remotely
    std::vector<std::pair<std::string, std::string>> renamedCards;  // temporary id -> real id
};

struct SyncError {
    int status = 0;  // HTTP status, 0 for transport failures (offline, timeout)
    std::string what;
};

class ChangeSink {
public:
    virtual ~ChangeSink() = default;
    virtual void onChanged(const ChangeSet& changes) = 0;
    virtual void onError(const SyncError& error) = 0;
};

struct SyncOptions {
    std::chrono::hours hotWindow{24 * 14};  // lists touched within this window are hot
    int alwaysHotLists = 3;                 // leftmost lists are hot even with no history
    int smallListThreshold = 50;            // lists this small are loaded eagerly
    bool prefetchCold = true;               // fetch cold lists in the background
    std::function<std::time_t()> now = [] { return std::time(nullptr); };
};

// Keeps the local cache fresh with tiered loading. All network and disk work runs on `executor`;
// reads below are cache lookups and are safe from any thread.
class SyncEngine {
public:
    SyncEngine(TrelloApi& api, Store& store, Executor& executor, ChangeSink& sink,
               SyncOptions options = {}, EventSource* events = nullptr);
    ~SyncEngine();  // waits for running jobs; queued ones become no-ops
    SyncEngine(const SyncEngine&) = delete;
    SyncEngine& operator=(const SyncEngine&) = delete;

    void start();                           // refresh the board list
    void openBoard(const std::string& id);  // lists, then hot lists' cards, then cold prefetch
    void openCard(const std::string& id);   // description + comments first, extras after
    void expandList(const std::string& id); // load a cold list now

    std::vector<Board> boards();
    std::vector<List> lists(const std::string& boardId);
    std::vector<Card> cards(const std::string& listId);
    std::optional<Card> card(const std::string& id);
    std::vector<Comment> comments(const std::string& cardId);
    std::vector<Label> labels(const std::string& boardId);
    std::vector<Member> members(const std::string& boardId);
    std::optional<CardExtras> extras(const std::string& cardId);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace cardinal
