#pragma once
// The project store: one SQLite database (.hs/store.db) holding immutable
// objects keyed by hash, mutable refs (branches, HEAD, working namespace),
// locks, and the derived index tables.
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/hash.h"

struct sqlite3;
struct sqlite3_stmt;

namespace hs {

struct Object {
  std::string kind;
  std::string body;
};

// Small RAII wrapper around a prepared statement.
class Stmt {
 public:
  Stmt(sqlite3* db, const char* sql);
  ~Stmt();
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  Stmt& bind(int i, const std::string& v);
  Stmt& bind_blob(int i, const std::string& v);
  Stmt& bind(int i, long long v);
  Stmt& bind_null(int i);
  bool step();  // true while rows remain
  void run();   // step to completion
  void reset();
  std::string text(int col) const;
  std::string blob(int col) const;
  long long integer(int col) const;
  bool is_null(int col) const;

 private:
  sqlite3* db_;
  sqlite3_stmt* st_ = nullptr;
};

// The store format this build reads and writes (see Store::Store).
constexpr int kStoreVersion = 4;

class Store {
 public:
  // Opens (and creates/migrates if needed) the database file.
  explicit Store(const std::string& db_path);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  Hash put(const std::string& kind, const std::string& body);
  std::optional<Object> get(const Hash& h) const;
  Object must_get(const Hash& h) const;
  bool has(const Hash& h) const;
  // Expands a unique prefix of at least 4 characters to a full hash.
  std::optional<Hash> expand(const std::string& prefix) const;

  std::optional<std::string> ref(const std::string& name) const;
  void set_ref(const std::string& name, const std::string& value);
  void delete_ref(const std::string& name);
  std::vector<std::pair<std::string, std::string>> refs_with_prefix(
      const std::string& prefix) const;

  void exec(const char* sql);
  sqlite3* db() const { return db_; }

  // Runs fn inside a transaction; rolls back if fn throws.
  void transaction(const std::function<void()>& fn);

 private:
  sqlite3* db_ = nullptr;
  int tx_depth_ = 0;
};

}  // namespace hs
