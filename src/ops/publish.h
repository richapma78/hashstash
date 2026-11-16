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

// Git-ignore rules: paths that live in hashstash (versioned, synced with the
// hub, rendered by hs render) but are never published to git. They are part
// of the namespace, so they are committed, pushed and pulled like code, and
// every agent and the hub's publisher see the same rules.
//
// Patterns are globs over project paths, like .gitignore: "secrets/" or
// "secrets" (a folder and everything in it), "config/local.toml", "*.env" (no
// slash: matches at any depth), "tools/**/scratch_*.py".
// A file that was published before it was ignored is removed from the git
// branch on the next publish; it stays in the git history already pushed.
std::string normalize_git_ignore(const std::string& pattern);
bool git_ignore_matches(const std::string& pattern, const std::string& path);
bool git_ignored(const Namespace& ns, const std::string& path);
// [{pattern, reason, files: [...]}] for the working namespace.
nlohmann::json git_ignore_list(Repo& repo);
// Adds a rule to the working namespace (commit to keep it). reason is required.
// Returns {pattern, reason, files}.
nlohmann::json git_ignore_add(Repo& repo, const std::string& pattern, const std::string& reason);
void git_ignore_remove(Repo& repo, const std::string& pattern);

}  // namespace hs
