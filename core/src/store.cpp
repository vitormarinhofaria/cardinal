#include "cardinal/store.h"

#include <sqlite3.h>

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace cardinal {
namespace {

constexpr int kSchemaVersion = 1;

[[noreturn]] void fail(sqlite3* db, const char* what) {
    throw std::runtime_error(std::string(what) + ": " + sqlite3_errmsg(db));
}

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) fail(db, "prepare");
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    ~Stmt() { sqlite3_finalize(s_); }

    void text(int i, const std::string& v) {
        sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    }
    void real(int i, double v) { sqlite3_bind_double(s_, i, v); }
    void integer(int i, sqlite3_int64 v) { sqlite3_bind_int64(s_, i, v); }

    bool step() {
        int rc = sqlite3_step(s_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        fail(db_, "step");
    }
    void run() { step(); sqlite3_reset(s_); }

    std::string col_text(int i) const {
        auto p = reinterpret_cast<const char*>(sqlite3_column_text(s_, i));
        return p ? std::string(p, static_cast<size_t>(sqlite3_column_bytes(s_, i))) : std::string();
    }
    double col_real(int i) const { return sqlite3_column_double(s_, i); }
    int col_int(int i) const { return sqlite3_column_int(s_, i); }
    sqlite3_int64 col_int64(int i) const { return sqlite3_column_int64(s_, i); }

private:
    sqlite3* db_;
    sqlite3_stmt* s_ = nullptr;
};

void exec(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) fail(db, "exec");
}

// Batches writes in one transaction.
class Tx {
public:
    explicit Tx(sqlite3* db) : db_(db) { exec(db_, "BEGIN"); }
    Tx(const Tx&) = delete;
    Tx& operator=(const Tx&) = delete;
    ~Tx() {
        if (!done_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        exec(db_, "COMMIT");
        done_ = true;
    }

private:
    sqlite3* db_;
    bool done_ = false;
};

int userVersion(sqlite3* db) {
    Stmt s(db, "PRAGMA user_version");
    s.step();
    return s.col_int(0);
}

void migrate(sqlite3* db) {
    if (userVersion(db) >= kSchemaVersion) return;
    exec(db, R"sql(
        CREATE TABLE IF NOT EXISTS boards(
            id TEXT PRIMARY KEY, name TEXT NOT NULL, closed INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS lists(
            id TEXT PRIMARY KEY, board_id TEXT NOT NULL, name TEXT NOT NULL,
            pos REAL NOT NULL, card_count INTEGER NOT NULL);
        CREATE INDEX IF NOT EXISTS lists_board ON lists(board_id);
        CREATE TABLE IF NOT EXISTS cards(
            id TEXT PRIMARY KEY, list_id TEXT NOT NULL, name TEXT NOT NULL, desc TEXT NOT NULL,
            pos REAL NOT NULL, label_ids TEXT NOT NULL, member_ids TEXT NOT NULL,
            comment_count INTEGER NOT NULL, date_last_activity TEXT NOT NULL);
        CREATE INDEX IF NOT EXISTS cards_list ON cards(list_id, pos);
        CREATE TABLE IF NOT EXISTS sync_cursor(
            board_id TEXT PRIMARY KEY, cursor TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS list_touch(
            list_id TEXT PRIMARY KEY, touched_at INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS pending_ops(
            id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, card_id TEXT NOT NULL,
            payload TEXT NOT NULL, created_at INTEGER NOT NULL);
    )sql");
    exec(db, ("PRAGMA user_version = " + std::to_string(kSchemaVersion)).c_str());
}

std::vector<std::string> parseIds(const std::string& text) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    return j.is_array() ? j.get<std::vector<std::string>>() : std::vector<std::string>{};
}

Card cardFromRow(const Stmt& s) {
    Card c;
    c.id = s.col_text(0);
    c.listId = s.col_text(1);
    c.name = s.col_text(2);
    c.desc = s.col_text(3);
    c.pos = s.col_real(4);
    c.labelIds = parseIds(s.col_text(5));
    c.memberIds = parseIds(s.col_text(6));
    c.commentCount = s.col_int(7);
    c.dateLastActivity = s.col_text(8);
    return c;
}

constexpr const char* kCardCols =
    "id, list_id, name, desc, pos, label_ids, member_ids, comment_count, date_last_activity";

}  // namespace

Store Store::open(const std::filesystem::path& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : "out of memory";
        sqlite3_close(db);
        throw std::runtime_error("open " + path.string() + ": " + msg);
    }
    Store s(db);
    exec(db, "PRAGMA journal_mode = WAL");
    exec(db, "PRAGMA synchronous = NORMAL");
    migrate(db);
    return s;
}

Store::Store(Store&& o) noexcept : db_(std::exchange(o.db_, nullptr)) {}

Store& Store::operator=(Store&& o) noexcept {
    if (this != &o) {
        sqlite3_close(db_);
        db_ = std::exchange(o.db_, nullptr);
    }
    return *this;
}

Store::~Store() { sqlite3_close(db_); }

void Store::upsertBoards(std::span<const Board> boards) {
    Tx tx(db_);
    Stmt s(db_, "INSERT OR REPLACE INTO boards(id, name, closed) VALUES(?,?,?)");
    for (const auto& b : boards) {
        s.text(1, b.id);
        s.text(2, b.name);
        s.integer(3, b.closed);
        s.run();
    }
    tx.commit();
}

void Store::upsertLists(std::span<const List> lists) {
    Tx tx(db_);
    Stmt s(db_,
           "INSERT OR REPLACE INTO lists(id, board_id, name, pos, card_count) VALUES(?,?,?,?,?)");
    for (const auto& l : lists) {
        s.text(1, l.id);
        s.text(2, l.boardId);
        s.text(3, l.name);
        s.real(4, l.pos);
        s.integer(5, l.cardCount);
        s.run();
    }
    tx.commit();
}

void Store::upsertCards(std::span<const Card> cards) {
    Tx tx(db_);
    Stmt s(db_, (std::string("INSERT OR REPLACE INTO cards(") + kCardCols +
                 ") VALUES(?,?,?,?,?,?,?,?,?)")
                    .c_str());
    for (const auto& c : cards) {
        s.text(1, c.id);
        s.text(2, c.listId);
        s.text(3, c.name);
        s.text(4, c.desc);
        s.real(5, c.pos);
        s.text(6, nlohmann::json(c.labelIds).dump());
        s.text(7, nlohmann::json(c.memberIds).dump());
        s.integer(8, c.commentCount);
        s.text(9, c.dateLastActivity);
        s.run();
    }
    tx.commit();
}

std::vector<Board> Store::boards() {
    Stmt s(db_, "SELECT id, name, closed FROM boards ORDER BY name");
    std::vector<Board> out;
    while (s.step()) out.push_back({s.col_text(0), s.col_text(1), s.col_int(2) != 0});
    return out;
}

std::vector<List> Store::lists(const std::string& boardId) {
    Stmt s(db_,
           "SELECT id, board_id, name, pos, card_count FROM lists WHERE board_id = ? ORDER BY pos");
    s.text(1, boardId);
    std::vector<List> out;
    while (s.step())
        out.push_back({s.col_text(0), s.col_text(1), s.col_text(2), s.col_real(3), s.col_int(4)});
    return out;
}

std::vector<Card> Store::cards(const std::string& listId) {
    Stmt s(db_, (std::string("SELECT ") + kCardCols + " FROM cards WHERE list_id = ? ORDER BY pos")
                    .c_str());
    s.text(1, listId);
    std::vector<Card> out;
    while (s.step()) out.push_back(cardFromRow(s));
    return out;
}

std::optional<Card> Store::card(const std::string& id) {
    Stmt s(db_, (std::string("SELECT ") + kCardCols + " FROM cards WHERE id = ?").c_str());
    s.text(1, id);
    if (!s.step()) return std::nullopt;
    return cardFromRow(s);
}

void Store::setSyncCursor(const std::string& boardId, const std::string& iso) {
    Stmt s(db_, "INSERT OR REPLACE INTO sync_cursor(board_id, cursor) VALUES(?,?)");
    s.text(1, boardId);
    s.text(2, iso);
    s.run();
}

std::optional<std::string> Store::syncCursor(const std::string& boardId) {
    Stmt s(db_, "SELECT cursor FROM sync_cursor WHERE board_id = ?");
    s.text(1, boardId);
    if (!s.step()) return std::nullopt;
    return s.col_text(0);
}

void Store::touchList(const std::string& listId, std::time_t when) {
    Stmt s(db_, "INSERT OR REPLACE INTO list_touch(list_id, touched_at) VALUES(?,?)");
    s.text(1, listId);
    s.integer(2, static_cast<sqlite3_int64>(when));
    s.run();
}

}  // namespace cardinal

namespace cardinal {

std::optional<std::time_t> Store::listTouchedAt(const std::string& listId) {
    Stmt s(db_, "SELECT touched_at FROM list_touch WHERE list_id = ?");
    s.text(1, listId);
    if (!s.step()) return std::nullopt;
    return static_cast<std::time_t>(s.col_int64(0));
}

}  // namespace cardinal
