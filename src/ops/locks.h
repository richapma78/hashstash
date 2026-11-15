#pragma once
// Hard, leased locks on definitions (by lineage id) or fragments (by crumb id).
// A lock blocks every other agent from changing the target until it is
// released or expires; any action by the holder renews all its locks.
#include <optional>
#include <string>
#include <vector>

#include "ops/repo.h"

namespace hs {

struct LockInfo {
  std::string target;  // lineage id or fragment id
  std::string name;    // qualified name when taken
  std::string holder;
  long long acquired = 0;
  long long expires = 0;
};

// Throws if another agent holds an unexpired lock on target.
LockInfo acquire_lock(Repo& repo, const std::string& target, const std::string& name);
// Returns false if no lock was held. Without force, only the holder may release.
bool release_lock(Repo& repo, const std::string& target, bool force = false);
std::optional<LockInfo> lock_on(Repo& repo, const std::string& target);  // unexpired only
std::vector<LockInfo> list_locks(Repo& repo);                             // unexpired only
// Nullopt if the current agent may change target, else the blocking lock.
std::optional<LockInfo> blocking_lock(Repo& repo, const std::string& target);
void renew_locks(Repo& repo);
int release_all(Repo& repo);  // releases every lock the current agent holds

}  // namespace hs
