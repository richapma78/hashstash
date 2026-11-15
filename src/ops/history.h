#pragma once
// Commits, the commit log, and the lifecycle history with rollback.
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "ops/diff.h"
#include "ops/repo.h"

namespace hs {

struct CommitResult {
  Hash commit;
  NamespaceDiff diff;
  int locks_released = 0;
};

// Commits the working namespace. message is required: what changed and why.
CommitResult commit(Repo& repo, const std::string& message, bool keep_locks = false);

// Commit log of the current branch (first parents), newest first. With a name,
// only commits that changed that definition (followed through renames).
nlohmann::json log(Repo& repo, int limit, const std::string& name = "");

struct HistoryEntry {
  long long id = 0;
  long long time = 0;
  std::string step, ns, commit, agent, message;
};
std::vector<HistoryEntry> history(Repo& repo, int limit);
std::optional<HistoryEntry> history_entry(Repo& repo, long long id);
// Restores the working state of a history entry. The current state stays in
// the history, so it can be compared or restored again.
HistoryEntry rollback(Repo& repo, long long id);

// Resolves a namespace reference: "work", "head", "@<history id>",
// "<commit hash prefix>", "head~N".
Hash resolve_ns(Repo& repo, const std::string& ref);

}  // namespace hs
