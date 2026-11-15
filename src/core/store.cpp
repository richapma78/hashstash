#include "core/store.h"

#include <sqlite3.h>

#include <stdexcept>

namespace hs {

// ---------------------------------------------------------------- Stmt

Stmt::Stmt(sqlite3* db, const char* sql) : db_(db) {
  if (sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK)
    throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(db) +
                             " in: " + sql);
}
Stmt::~Stmt() { sqlite3_finalize(st_); }
Stmt& Stmt::bind(int i, const std::string& v) {
  sqlite3_bind_text(st_, i, v.data(), (int)v.size(), SQLITE_TRANSIENT);
  return *this;
}
Stmt& Stmt::bind_blob(int i, const std::string& v) {
  sqlite3_bind_blob(st_, i, v.data(), (int)v.size(), SQLITE_TRANSIENT);
  return *this;
}
Stmt& Stmt::bind(int i, long long v) {
  sqlite3_bind_int64(st_, i, v);
  return *this;
}
Stmt& Stmt::bind_null(int i) {
  sqlite3_bind_null(st_, i);
  return *this;
}
bool Stmt::step() {
  int rc = sqlite3_step(st_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  throw std::runtime_error(std::string("sqlite step: ") + sqlite3_errmsg(db_));
}
void Stmt::run() {
  while (step()) {
  }
}
void Stmt::reset() {
  sqlite3_reset(st_);
  sqlite3_clear_bindings(st_);
}
std::string Stmt::text(int col) const {
  auto p = (const char*)sqlite3_column_text(st_, col);
  return p ? std::string(p, sqlite3_column_bytes(st_, col)) : std::string();
}
std::string Stmt::blob(int col) const {
  auto p = (const char*)sqlite3_column_blob(st_, col);
  return p ? std::string(p, sqlite3_column_bytes(st_, col)) : std::string();
}
long long Stmt::integer(int col) const { return sqlite3_column_int64(st_, col); }
bool Stmt::is_null(int col) const { return sqlite3_column_type(st_, col) == SQLITE_NULL; }

// ---------------------------------------------------------------- Store

static const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS object (
  hash TEXT PRIMARY KEY NOT NULL,
  kind TEXT NOT NULL,
  body BLOB NOT NULL
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS ref (name TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;

-- Locks are leases on a definition's lineage id (or a fragment id).
CREATE TABLE IF NOT EXISTS lock (
  target TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  holder TEXT NOT NULL,
  acquired INTEGER NOT NULL,
  expires INTEGER NOT NULL
) WITHOUT ROWID;

-- Per-definition analysis, keyed by content hash: computed once, kept forever.
CREATE TABLE IF NOT EXISTS def_info (
  hash TEXT PRIMARY KEY,
  lang TEXT NOT NULL,
  kind TEXT NOT NULL,
  name TEXT NOT NULL,
  lines INTEGER NOT NULL,
  signature TEXT NOT NULL,
  doc TEXT NOT NULL
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS def_ref (
  hash TEXT NOT NULL,     -- the calling definition (or file module code)
  name TEXT NOT NULL,     -- what it calls, as written: "foo", "self.bar", "os.path.join"
  kind TEXT NOT NULL,     -- "call" for now
  line INTEGER NOT NULL   -- 1-based line within the definition's own text
);
CREATE INDEX IF NOT EXISTS def_ref_hash ON def_ref(hash);
CREATE TABLE IF NOT EXISTS def_import (
  hash TEXT NOT NULL,
  module TEXT NOT NULL,
  name TEXT NOT NULL,
  alias TEXT NOT NULL,
  level INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS def_import_hash ON def_import(hash);

-- Resolved call graph for the working namespace; rebuilt with the name index.
-- A file's module-level code is the node "<file>:<module>".
CREATE TABLE IF NOT EXISTS edge (
  caller TEXT NOT NULL,
  callee TEXT NOT NULL,      -- qualified name, or "" when unresolved
  name TEXT NOT NULL,        -- as written in the caller
  line INTEGER NOT NULL,
  confidence TEXT NOT NULL   -- exact | likely | ambiguous | external
);
CREATE INDEX IF NOT EXISTS edge_caller ON edge(caller);
CREATE INDEX IF NOT EXISTS edge_callee ON edge(callee);
CREATE TABLE IF NOT EXISTS analyzed (hash TEXT PRIMARY KEY) WITHOUT ROWID;

-- The name index for the working namespace; rebuilt whenever it changes.
CREATE TABLE IF NOT EXISTS name_index (
  qname TEXT PRIMARY KEY,
  short TEXT NOT NULL,
  file TEXT NOT NULL,
  hash TEXT NOT NULL,
  lineage TEXT NOT NULL,
  kind TEXT NOT NULL,
  lang TEXT NOT NULL,
  parent TEXT NOT NULL,
  seq INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS name_index_short ON name_index(short);
CREATE INDEX IF NOT EXISTS name_index_file ON name_index(file);
CREATE INDEX IF NOT EXISTS name_index_hash ON name_index(hash);
CREATE VIRTUAL TABLE IF NOT EXISTS name_fts USING fts5(qname, short, signature, doc, tokenize='unicode61 tokenchars ''_''');

-- Lifecycle history: every change to a branch's working state (import, update,
-- commit, verify, rollback...). Rolling back adds an entry, so nothing is lost.
CREATE TABLE IF NOT EXISTS history (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  branch TEXT NOT NULL,
  time INTEGER NOT NULL,
  step TEXT NOT NULL,
  ns TEXT NOT NULL,
  commit_hash TEXT NOT NULL,
  agent TEXT NOT NULL,
  message TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS history_branch ON history(branch, id);

-- Verification results, keyed by the namespace they were run against.
CREATE TABLE IF NOT EXISTS verification (
  ns TEXT PRIMARY KEY,
  status TEXT NOT NULL,      -- passed | failed
  time INTEGER NOT NULL,
  agent TEXT NOT NULL,
  report TEXT NOT NULL       -- JSON
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS test_result (
  suite TEXT NOT NULL,       -- suite hash of the definition under test
  test TEXT NOT NULL,        -- test qualified name
  test_hash TEXT NOT NULL,
  status TEXT NOT NULL,      -- passed | failed | error
  time INTEGER NOT NULL,
  output TEXT NOT NULL,
  PRIMARY KEY (suite, test)
) WITHOUT ROWID;

-- Files written by the last render into each output folder.
CREATE TABLE IF NOT EXISTS render_manifest (
  out_dir TEXT NOT NULL,
  path TEXT NOT NULL,
  hash TEXT NOT NULL,
  PRIMARY KEY (out_dir, path)
) WITHOUT ROWID;

-- Reviews of commits by people and AI agents. Append-only; synced with the
-- hub by id (rowid orders them on the hub for incremental pulls).
CREATE TABLE IF NOT EXISTS review (
  id TEXT PRIMARY KEY,
  commit_hash TEXT NOT NULL,
  reviewer TEXT NOT NULL,
  kind TEXT NOT NULL,        -- human | ai
  verdict TEXT NOT NULL,     -- approved | changes_requested | comment
  summary TEXT NOT NULL,
  comments TEXT NOT NULL,    -- JSON [{name, line, text}]
  time INTEGER NOT NULL,
  synced INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS review_commit ON review(commit_hash);

-- The review flag per commit, kept current as reviews arrive:
-- level = unreviewed | ai (only AI reviewed) | human (a person reviewed too).
CREATE TABLE IF NOT EXISTS commit_review (
  commit_hash TEXT PRIMARY KEY,
  level TEXT NOT NULL,
  ai_reviewed INTEGER NOT NULL,
  human_reviewed INTEGER NOT NULL,
  human_approved INTEGER NOT NULL,
  ai_verdict TEXT NOT NULL,     -- latest AI verdict, or ''
  human_verdict TEXT NOT NULL,  -- latest human verdict, or ''
  updated INTEGER NOT NULL
) WITHOUT ROWID;

-- Pushes accepted (on a hub) or made (on a client).
CREATE TABLE IF NOT EXISTS push_log (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  branch TEXT NOT NULL,
  old_head TEXT NOT NULL,
  new_head TEXT NOT NULL,
  agent TEXT NOT NULL,
  time INTEGER NOT NULL
);
)SQL";

Store::Store(const std::string& db_path) {
  if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
    std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
    sqlite3_close(db_);
    throw std::runtime_error("cannot open store " + db_path + ": " + msg);
  }
  sqlite3_busy_timeout(db_, 10000);
  exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
  exec(kSchema);
  // Migrations for stores created by earlier versions.
  try {
    exec("ALTER TABLE lock ADD COLUMN origin TEXT NOT NULL DEFAULT ''");  // '' = local, else remote name
  } catch (...) {
  }
  exec("INSERT OR IGNORE INTO meta(key, value) VALUES ('schema_version', '1')");
}

Store::~Store() { sqlite3_close(db_); }

void Store::exec(const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "?";
    sqlite3_free(err);
    throw std::runtime_error("sqlite: " + msg);
  }
}

void Store::transaction(const std::function<void()>& fn) {
  if (tx_depth_ > 0) {  // nested: join the outer transaction
    fn();
    return;
  }
  exec("BEGIN IMMEDIATE");
  ++tx_depth_;
  try {
    fn();
  } catch (...) {
    --tx_depth_;
    exec("ROLLBACK");
    throw;
  }
  --tx_depth_;
  exec("COMMIT");
}

Hash Store::put(const std::string& kind, const std::string& body) {
  Hash h = hash_bytes(kind, body);
  Stmt st(db_, "INSERT OR IGNORE INTO object(hash, kind, body) VALUES (?, ?, ?)");
  st.bind(1, h).bind(2, kind).bind_blob(3, body).run();
  return h;
}

std::optional<Object> Store::get(const Hash& h) const {
  Stmt st(db_, "SELECT kind, body FROM object WHERE hash = ?");
  st.bind(1, h);
  if (!st.step()) return std::nullopt;
  return Object{st.text(0), st.blob(1)};
}

Object Store::must_get(const Hash& h) const {
  auto o = get(h);
  if (!o) throw std::runtime_error("missing object #" + short_hash(h));
  return *o;
}

bool Store::has(const Hash& h) const {
  Stmt st(db_, "SELECT 1 FROM object WHERE hash = ?");
  st.bind(1, h);
  return st.step();
}

std::optional<Hash> Store::expand(const std::string& prefix) const {
  if (prefix.size() < 4) return std::nullopt;
  Stmt st(db_, "SELECT hash FROM object WHERE hash >= ? AND hash < ? LIMIT 2");
  st.bind(1, prefix).bind(2, prefix + "\x7f");
  std::optional<Hash> found;
  while (st.step()) {
    if (found) return std::nullopt;  // ambiguous
    found = st.text(0);
  }
  return found;
}

std::optional<std::string> Store::ref(const std::string& name) const {
  Stmt st(db_, "SELECT value FROM ref WHERE name = ?");
  st.bind(1, name);
  if (!st.step()) return std::nullopt;
  return st.text(0);
}

void Store::set_ref(const std::string& name, const std::string& value) {
  Stmt st(db_, "INSERT INTO ref(name, value) VALUES (?, ?) "
               "ON CONFLICT(name) DO UPDATE SET value = excluded.value");
  st.bind(1, name).bind(2, value).run();
}

void Store::delete_ref(const std::string& name) {
  Stmt st(db_, "DELETE FROM ref WHERE name = ?");
  st.bind(1, name).run();
}

std::vector<std::pair<std::string, std::string>> Store::refs_with_prefix(
    const std::string& prefix) const {
  Stmt st(db_, "SELECT name, value FROM ref WHERE name >= ? AND name < ? ORDER BY name");
  st.bind(1, prefix).bind(2, prefix + "\x7f");
  std::vector<std::pair<std::string, std::string>> out;
  while (st.step()) out.emplace_back(st.text(0), st.text(1));
  return out;
}

}  // namespace hs
