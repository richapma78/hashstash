#pragma once
// Publishing: renders a branch head into a git working copy, commits the
// result with the hashstash commit messages since the last publish, and pushes
// it to a git remote. Each publish is a build of the project as normal files,
// for compilers, CI, and people browsing on GitHub.
#include <nlohmann/json.hpp>

#include "ops/repo.h"

namespace hs {

// Publishes the head of branch (default: current branch) per [publish] config.
// Only files hashstash manages are written or removed; anything else in the
// git working copy (README, CI config) is left alone.
nlohmann::json publish(Repo& repo, const std::string& branch = "");
bool publish_on_push(const Repo& repo);

}  // namespace hs
