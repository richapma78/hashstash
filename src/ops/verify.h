#pragma once
// Verification: the gate between committing and pushing. Checks the policy
// (tests linked, size limits) for changed definitions, renders the working
// namespace, runs the linked tests, and records the result against the
// namespace hash. A push only accepts verified namespaces.
#include <nlohmann/json.hpp>

#include "ops/repo.h"

namespace hs {

// all = check every definition, not just those changed since HEAD.
nlohmann::json verify(Repo& repo, bool all = false);
// "passed", "failed", or "" if never verified.
std::string verification_status(Repo& repo, const Hash& ns);

}  // namespace hs
