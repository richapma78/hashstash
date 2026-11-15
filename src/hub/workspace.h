#pragma once
// Hub workspaces: a full hashstash project (store, index, working state) for a
// hosted project, so the ops library -- verify, render, publish, the MCP tools
// and the review pages -- runs on the hub exactly as it does on a client.
//
// Backends are storage only. One whose project storage already is a hashstash
// project (SQLite) offers it through ProjectStore::native_repo(), and the
// workspace uses it directly. Any other backend gets a cached workspace: a
// local project under the cache folder, brought up to date from the backend's
// objects, refs, verification reports, reviews and push log before each use,
// whose own changes (commits and edits made through the hub's MCP tools,
// reviews recorded on the pages) are written back after. A hub server keeps
// nothing it can't rebuild, so any number of them can share one backend.
#include <memory>
#include <string>

#include "core/util.h"
#include "hub/backend.h"
#include "ops/repo.h"

namespace hs {

class Workspace {
 public:
  // cache_dir: where a cached workspace keeps its project ("" = never cache;
  // the backend must then offer native_repo()). force_cache ignores
  // native_repo(), to run the way a hub on any other backend runs.
  Workspace(std::shared_ptr<ProjectStore> p, const fs::path& cache_dir, bool force_cache = false);
  Repo& repo() { return *repo_; }
  bool cached() const { return !native_; }

  // Brings the workspace up to date with the backend. Cheap when nothing moved.
  void refresh();
  // Copies everything reachable from h (commit or namespace) into the
  // workspace. Nothing to do for a native workspace.
  void fetch(const Hash& h);
  // Writes the workspace's own changes back to the backend. Throws if the
  // backend moved meanwhile (the caller refreshes and retries).
  void save(const std::string& agent);

 private:
  void follow_heads();
  void open_cache();
  std::shared_ptr<ProjectStore> p_;
  fs::path dir_;
  Repo* native_ = nullptr;
  std::unique_ptr<Repo> owned_;
  Repo* repo_ = nullptr;
};

}  // namespace hs
