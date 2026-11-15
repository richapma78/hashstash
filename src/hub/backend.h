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
//   coordination  locks (leases with expiry), work items, requests and the
//                 event feed (same database; see ops/coord.h)
//   verification  results keyed by namespace hash
//   jobs          queued background work (render, publish), so pushes return
//                 fast and many stateless hub servers can share the load
//   reviews       append-only records keyed by id, plus the push log
//   sessions      MCP session state, so any hub server can serve any session
//
// The full ops library (verify, render, publish, MCP tools, review pages) runs
// on a Workspace (hub/workspace.h) built from these interfaces, so no feature
// depends on which backend is used.
#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/store.h"
#include "ops/coord.h"

namespace hs {

class Repo;

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

  // Other refs (e.g. "work/<branch>": the hub workspace's uncommitted state),
  // also changed only by compare-and-swap ("" = absent).
  virtual std::string ref(const std::string& name) = 0;
  virtual bool cas_ref(const std::string& name, const std::string& old_value, const std::string& new_value) = 0;

  // Locks, work items, requests, events.
  virtual Coordination& coord() = 0;

  // The project's config.toml text (publish settings and so on).
  virtual std::string config_text() = 0;

  // Verification
  virtual void set_verification(const Hash& ns, const std::string& status, const std::string& report) = 0;
  virtual std::string verification(const Hash& ns) = 0;
  virtual std::string verification_report(const Hash& ns) = 0;  // JSON, or "" if none

  // Reviews (append-only, keyed by id) and the log of accepted pushes
  virtual void put_review(const nlohmann::json& review) = 0;
  virtual nlohmann::json reviews_after(long long seq) = 0;  // {reviews, last}
  virtual void log_push(const std::string& branch, const Hash& old_head, const Hash& new_head,
                        const std::string& agent) = 0;
  virtual nlohmann::json pushes_after(long long id) = 0;  // {pushes: [{id, branch, old, new, agent, time}], last}

  // Optional fast path: a backend whose storage already is a hashstash project
  // returns it, and the hub's workspace uses it directly instead of a cache.
  // Returning nullptr is always correct.
  virtual Repo* native_repo() { return nullptr; }

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
  // MCP sessions (null if unknown or expired).
  virtual void save_session(const std::string& id, const nlohmann::json& data, long long expires) = 0;
  virtual nlohmann::json load_session(const std::string& id) = 0;
};

// SQLite backend: one hashstash project store per hosted project under root.
std::unique_ptr<HubBackend> make_sqlite_backend(const std::string& root);

}  // namespace hs
