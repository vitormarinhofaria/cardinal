#pragma once

#include <ctime>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cardinal/model.h"

struct sqlite3;

namespace cardinal {

// SQLite-backed local cache. Not thread-safe: callers serialize access (one Store per thread, or
// an external mutex).
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

    std::vector<Board> boards();
    std::vector<List> lists(const std::string& boardId);
    std::vector<Card> cards(const std::string& listId);
    std::optional<Card> card(const std::string& id);

    void setSyncCursor(const std::string& boardId, const std::string& iso);
    std::optional<std::string> syncCursor(const std::string& boardId);

    void touchList(const std::string& listId, std::time_t when);
    std::optional<std::time_t> listTouchedAt(const std::string& listId);

private:
    explicit Store(sqlite3* db) : db_(db) {}
    sqlite3* db_ = nullptr;
};

}  // namespace cardinal
