#pragma once
// Storage interface for the hub. The hub's HTTP and MCP layers talk only to
// these interfaces, so the storage can be swapped without touching them.
//
// Each concern has the consistency it actually needs (the way large code hosts
// split storage):
//   objects       immutable, content-addressed: any key-value or blob store
//                 (local SQLite today; S3/Azure Blob sharded by hash prefix later)
//   refs          branch heads, updated only by compare-and-swap: needs a
//                 strongly consistent database (SQLite today; Postgres later)
//   locks         leases with expiry (same database, or Redis)
//   verification  results keyed by namespace hash
//   jobs          queued background work (render, publish), so pushes return
//                 fast and many stateless hub servers can share the load
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/store.h"

namespace hs {

struct HubLock {
  std::string target, name, holder;
  long long expires = 0;
};

struct HubJob {
  long long id = 0;
  std::string project, kind, arg;  // e.g. kind "publish", arg "main"
};

class ProjectStore {
 public:
  virtual ~ProjectStore() = default;
  virtual std::string name() const = 0;

  // Objects
  virtual bool has(const Hash& h) = 0;
  virtual std::optional<Object> get(const Hash& h) = 0;
  virtual void put(const Hash& h, const Object& o) = 0;  // caller has verified h

  // Refs
  virtual std::map<std::string, Hash> branches() = 0;
  // Sets branch to new_head only if it is currently old_head ("" = absent).
  virtual bool cas_branch(const std::string& branch, const Hash& old_head, const Hash& new_head) = 0;

  // Locks
  virtual std::vector<HubLock> locks() = 0;  // unexpired
  // Fails (returns the blocking lock) if someone else holds target.
  virtual std::optional<HubLock> acquire(const HubLock& l) = 0;
  virtual bool release(const std::string& target, const std::string& holder, bool force) = 0;

  // Verification
  virtual void set_verification(const Hash& ns, const std::string& status, const std::string& report) = 0;
  virtual std::string verification(const Hash& ns) = 0;

  // Batch work: everything between begin/commit is atomic.
  virtual void begin() = 0;
  virtual void commit() = 0;
  virtual void rollback() = 0;
};

class HubBackend {
 public:
  virtual ~HubBackend() = default;
  virtual std::vector<std::string> projects() = 0;
  // nullptr if it doesn't exist and create is false.
  virtual std::shared_ptr<ProjectStore> project(const std::string& name, bool create) = 0;
  // Background jobs, shared by all hub servers using this backend.
  virtual void enqueue(const HubJob& j) = 0;
  virtual std::optional<HubJob> next_job() = 0;  // claims one
  virtual void finish_job(long long id, const std::string& result) = 0;
};

// SQLite backend: one hashstash project store per hosted project under root.
std::unique_ptr<HubBackend> make_sqlite_backend(const std::string& root);

}  // namespace hs
