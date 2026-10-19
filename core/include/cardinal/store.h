#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cardinal/model.h"

struct sqlite3;

namespace cardinal {

// A write the user made that Trello has not confirmed yet.
struct PendingOp {
    std::int64_t id = 0;
    std::string kind;
    std::string cardId;
    std::string payload;  // JSON owned by the write queue
    std::time_t createdAt = 0;
};

}  // namespace cardinal

namespace cardinal {

// SQLite-backed local cache. Every public method is safe to call from any thread.
class Store {
public:
    static Store open(const std::filesystem::path& path);

    Store(Store&&) noexcept;
    Store& operator=(Store&&) noexcept;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    ~Store();

    void upsertBoards(std::span<const Board> boards);
    void upsertLists(std::span<const List> lists);
    void upsertCards(std::span<const Card> cards);

    // List fetches carry no description: update everything except `desc` of cards already cached.
    void upsertCardSummaries(std::span<const Card> cards);

    // Remove cached rows that the server no longer has. Return the ids of removed cards where
    // cards are removed.
    std::vector<std::string> pruneListCards(const std::string& listId,
                                            std::span<const std::string> keepIds);
    void pruneBoardLists(const std::string& boardId, std::span<const std::string> keepIds);
    void pruneBoards(std::span<const std::string> keepIds);
    void deleteCard(const std::string& cardId);

    void replaceLabels(const std::string& boardId, std::span<const Label> labels);
    void replaceMembers(const std::string& boardId, std::span<const Member> members);
    void replaceComments(const std::string& cardId, std::span<const Comment> comments);
    std::vector<Label> labels(const std::string& boardId);
    std::vector<Member> members(const std::string& boardId);
    std::vector<Comment> comments(const std::string& cardId);

    std::vector<Board> boards();
    std::vector<List> lists(const std::string& boardId);
    std::vector<Card> cards(const std::string& listId);
    std::optional<Card> card(const std::string& id);

    void setSyncCursor(const std::string& boardId, const std::string& iso);
    std::optional<std::string> syncCursor(const std::string& boardId);

    std::int64_t addPendingOp(const std::string& kind, const std::string& cardId,
                              const std::string& payload, std::time_t createdAt);
    std::vector<PendingOp> pendingOps();  // oldest first
    void updatePendingOp(std::int64_t id, const std::string& cardId, const std::string& payload);
    void removePendingOp(std::int64_t id);

    void touchList(const std::string& listId, std::time_t when);
    std::optional<std::time_t> listTouchedAt(const std::string& listId);

private:
    explicit Store(sqlite3* db) : db_(db) {}
    sqlite3* db_ = nullptr;
    std::unique_ptr<std::mutex> mu_ = std::make_unique<std::mutex>();
};

}  // namespace cardinal
