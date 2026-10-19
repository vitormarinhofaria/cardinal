#include "cardinal/store.h"

#include <sqlite3.h>

#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <utility>

namespace cardinal {
namespace {

constexpr int kSchemaVersion = 2;

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
    const int version = userVersion(db);
    if (version >= kSchemaVersion) return;
    Tx tx(db);
    if (version < 1) {
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
    }
    if (version < 2) {
        exec(db, R"sql(
            CREATE TABLE IF NOT EXISTS labels(
                board_id TEXT NOT NULL, id TEXT NOT NULL, name TEXT NOT NULL, color TEXT NOT NULL,
                ord INTEGER NOT NULL, PRIMARY KEY(board_id, id));
            CREATE TABLE IF NOT EXISTS members(
                board_id TEXT NOT NULL, id TEXT NOT NULL, full_name TEXT NOT NULL,
                initials TEXT NOT NULL, ord INTEGER NOT NULL, PRIMARY KEY(board_id, id));
            CREATE TABLE IF NOT EXISTS comments(
                card_id TEXT NOT NULL, id TEXT NOT NULL, author_id TEXT NOT NULL,
                text TEXT NOT NULL, date TEXT NOT NULL, ord INTEGER NOT NULL,
                PRIMARY KEY(card_id, id));
        )sql");
    }
    exec(db, ("PRAGMA user_version = " + std::to_string(kSchemaVersion)).c_str());
    tx.commit();
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

std::vector<std::string> idsOf(sqlite3* db, const char* sql, const std::string& arg) {
    Stmt s(db, sql);
    s.text(1, arg);
    std::vector<std::string> out;
    while (s.step()) out.push_back(s.col_text(0));
    return out;
}

void deleteCardRows(sqlite3* db, const std::string& cardId) {
    Stmt c(db, "DELETE FROM cards WHERE id = ?");
    c.text(1, cardId);
    c.run();
    Stmt m(db, "DELETE FROM comments WHERE card_id = ?");
    m.text(1, cardId);
    m.run();
}

std::vector<std::string> pruneListCardsRows(sqlite3* db, const std::string& listId,
                                            const std::set<std::string>& keep) {
    std::vector<std::string> removed;
    for (auto& id : idsOf(db, "SELECT id FROM cards WHERE list_id = ?", listId)) {
        if (keep.count(id)) continue;
        deleteCardRows(db, id);
        removed.push_back(std::move(id));
    }
    return removed;
}

void pruneBoardListsRows(sqlite3* db, const std::string& boardId, const std::set<std::string>& keep) {
    for (const auto& id : idsOf(db, "SELECT id FROM lists WHERE board_id = ?", boardId)) {
        if (keep.count(id)) continue;
        pruneListCardsRows(db, id, {});
        Stmt d(db, "DELETE FROM lists WHERE id = ?");
        d.text(1, id);
        d.run();
    }
}

std::set<std::string> toSet(std::span<const std::string> ids) {
    return std::set<std::string>(ids.begin(), ids.end());
}

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

Store::Store(Store&& o) noexcept
    : db_(std::exchange(o.db_, nullptr)), mu_(std::move(o.mu_)) {}

Store& Store::operator=(Store&& o) noexcept {
    if (this != &o) {
        sqlite3_close(db_);
        db_ = std::exchange(o.db_, nullptr);
        mu_ = std::move(o.mu_);
    }
    return *this;
}

Store::~Store() { sqlite3_close(db_); }

void Store::upsertBoards(std::span<const Board> boards) {
    std::lock_guard lk(*mu_);
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
    std::lock_guard lk(*mu_);
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
    std::lock_guard lk(*mu_);
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
    std::lock_guard lk(*mu_);
    Stmt s(db_, "SELECT id, name, closed FROM boards ORDER BY name");
    std::vector<Board> out;
    while (s.step()) out.push_back({s.col_text(0), s.col_text(1), s.col_int(2) != 0});
    return out;
}

std::vector<List> Store::lists(const std::string& boardId) {
    std::lock_guard lk(*mu_);
    Stmt s(db_,
           "SELECT id, board_id, name, pos, card_count FROM lists WHERE board_id = ? ORDER BY pos");
    s.text(1, boardId);
    std::vector<List> out;
    while (s.step())
        out.push_back({s.col_text(0), s.col_text(1), s.col_text(2), s.col_real(3), s.col_int(4)});
    return out;
}

std::vector<Card> Store::cards(const std::string& listId) {
    std::lock_guard lk(*mu_);
    Stmt s(db_, (std::string("SELECT ") + kCardCols + " FROM cards WHERE list_id = ? ORDER BY pos")
                    .c_str());
    s.text(1, listId);
    std::vector<Card> out;
    while (s.step()) out.push_back(cardFromRow(s));
    return out;
}

std::optional<Card> Store::card(const std::string& id) {
    std::lock_guard lk(*mu_);
    Stmt s(db_, (std::string("SELECT ") + kCardCols + " FROM cards WHERE id = ?").c_str());
    s.text(1, id);
    if (!s.step()) return std::nullopt;
    return cardFromRow(s);
}

void Store::setSyncCursor(const std::string& boardId, const std::string& iso) {
    std::lock_guard lk(*mu_);
    Stmt s(db_, "INSERT OR REPLACE INTO sync_cursor(board_id, cursor) VALUES(?,?)");
    s.text(1, boardId);
    s.text(2, iso);
    s.run();
}

std::optional<std::string> Store::syncCursor(const std::string& boardId) {
    std::lock_guard lk(*mu_);
    Stmt s(db_, "SELECT cursor FROM sync_cursor WHERE board_id = ?");
    s.text(1, boardId);
    if (!s.step()) return std::nullopt;
    return s.col_text(0);
}

void Store::touchList(const std::string& listId, std::time_t when) {
    std::lock_guard lk(*mu_);
    Stmt s(db_, "INSERT OR REPLACE INTO list_touch(list_id, touched_at) VALUES(?,?)");
    s.text(1, listId);
    s.integer(2, static_cast<sqlite3_int64>(when));
    s.run();
}

}  // namespace cardinal

namespace cardinal {

std::optional<std::time_t> Store::listTouchedAt(const std::string& listId) {
    std::lock_guard lk(*mu_);
    Stmt s(db_, "SELECT touched_at FROM list_touch WHERE list_id = ?");
    s.text(1, listId);
    if (!s.step()) return std::nullopt;
    return static_cast<std::time_t>(s.col_int64(0));
}

}  // namespace cardinal

namespace cardinal {

void Store::upsertCardSummaries(std::span<const Card> cards) {
    std::lock_guard lk(*mu_);
    Tx tx(db_);
    Stmt s(db_, (std::string("INSERT INTO cards(") + kCardCols +
                 ") VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
                 "list_id=excluded.list_id, name=excluded.name, pos=excluded.pos, "
                 "label_ids=excluded.label_ids, member_ids=excluded.member_ids, "
                 "comment_count=excluded.comment_count, "
                 "date_last_activity=excluded.date_last_activity")
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

std::vector<std::string> Store::pruneListCards(const std::string& listId,
                                               std::span<const std::string> keepIds) {
    std::lock_guard lk(*mu_);
    Tx tx(db_);
    auto removed = pruneListCardsRows(db_, listId, toSet(keepIds));
    tx.commit();
    return removed;
}

void Store::pruneBoardLists(const std::string& boardId, std::span<const std::string> keepIds) {
    std::lock_guard lk(*mu_);
    Tx tx(db_);
    pruneBoardListsRows(db_, boardId, toSet(keepIds));
    tx.commit();
}

void Store::pruneBoards(std::span<const std::string> keepIds) {
    std::lock_guard lk(*mu_);
    Tx tx(db_);
    const auto keep = toSet(keepIds);
    std::vector<std::string> gone;
    {
        Stmt s(db_, "SELECT id FROM boards");
        while (s.step())
            if (!keep.count(s.col_text(0))) gone.push_back(s.col_text(0));
    }
    for (const auto& id : gone) {
        pruneBoardListsRows(db_, id, {});
        Stmt d(db_, "DELETE FROM boards WHERE id = ?");
        d.text(1, id);
        d.run();
    }
    tx.commit();
}

void Store::deleteCard(const std::string& cardId) {
    std::lock_guard lk(*mu_);
    Tx tx(db_);
    deleteCardRows(db_, cardId);
    tx.commit();
}

void Store::replaceLabels(const std::string& boardId, std::span<const Label> labels) {
    std::lock_guard lk(*mu_);
    Tx tx(db_);
    Stmt del(db_, "DELETE FROM labels WHERE board_id = ?");
    del.text(1, boardId);
    del.run();
    Stmt ins(db_, "INSERT INTO labels(board_id, id, name, color, ord) VALUES(?,?,?,?,?)");
    int ord = 0;
    for (const auto& l : labels) {
        ins.text(1, boardId);
        ins.text(2, l.id);
        ins.text(3, l.name);
        ins.text(4, l.color);
        ins.integer(5, ord++);
        ins.run();
    }
    tx.commit();
}

void Store::replaceMembers(const std::string& boardId, std::span<const Member> members) {
    std::lock_guard lk(*mu_);
    Tx tx(db_);
    Stmt del(db_, "DELETE FROM members WHERE board_id = ?");
    del.text(1, boardId);
    del.run();
    Stmt ins(db_, "INSERT INTO members(board_id, id, full_name, initials, ord) VALUES(?,?,?,?,?)");
    int ord = 0;
    for (const auto& m : members) {
        ins.text(1, boardId);
        ins.text(2, m.id);
        ins.text(3, m.fullName);
        ins.text(4, m.initials);
        ins.integer(5, ord++);
        ins.run();
    }
    tx.commit();
}

void Store::replaceComments(const std::string& cardId, std::span<const Comment> comments) {
    std::lock_guard lk(*mu_);
    Tx tx(db_);
    Stmt del(db_, "DELETE FROM comments WHERE card_id = ?");
    del.text(1, cardId);
    del.run();
    Stmt ins(db_,
             "INSERT OR REPLACE INTO comments(card_id, id, author_id, text, date, ord) "
             "VALUES(?,?,?,?,?,?)");
    int ord = 0;
    for (const auto& c : comments) {
        ins.text(1, cardId);
        ins.text(2, c.id);
        ins.text(3, c.authorId);
        ins.text(4, c.text);
        ins.text(5, c.date);
        ins.integer(6, ord++);
        ins.run();
    }
    tx.commit();
}

std::vector<Label> Store::labels(const std::string& boardId) {
    std::lock_guard lk(*mu_);
    Stmt s(db_, "SELECT id, name, color FROM labels WHERE board_id = ? ORDER BY ord");
    s.text(1, boardId);
    std::vector<Label> out;
    while (s.step()) out.push_back({s.col_text(0), s.col_text(1), s.col_text(2)});
    return out;
}

std::vector<Member> Store::members(const std::string& boardId) {
    std::lock_guard lk(*mu_);
    Stmt s(db_, "SELECT id, full_name, initials FROM members WHERE board_id = ? ORDER BY ord");
    s.text(1, boardId);
    std::vector<Member> out;
    while (s.step()) out.push_back({s.col_text(0), s.col_text(1), s.col_text(2)});
    return out;
}

std::vector<Comment> Store::comments(const std::string& cardId) {
    std::lock_guard lk(*mu_);
    Stmt s(db_, "SELECT id, author_id, text, date FROM comments WHERE card_id = ? ORDER BY ord");
    s.text(1, cardId);
    std::vector<Comment> out;
    while (s.step()) out.push_back({s.col_text(0), s.col_text(1), s.col_text(2), s.col_text(3)});
    return out;
}

}  // namespace cardinal
