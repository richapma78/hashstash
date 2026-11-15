#pragma once
// A project: its .hs/ folder, store, config, current branch and working namespace.
//
// Refs in the store:
//   HEAD            current branch name
//   branch/<name>   commit hash ("" for a branch with no commits yet)
//   work/<name>     working namespace hash for that branch (uncommitted state)
#include <memory>
#include <optional>
#include <string>

#include "core/model.h"
#include "core/store.h"
#include "core/util.h"

namespace hs {

struct Config {
  std::string project;
  std::string author;
  int warn_lines = 60;
  int max_lines = 0;
  int lock_minutes = 15;
  std::vector<std::string> ignore;  // extra glob patterns for import
  std::string tests_require = "warn";      // off | warn | block
  std::vector<std::string> tests_exempt;   // short-name globs that need no tests
  std::string test_command_python = "python -m pytest -q {tests}";
  int tests_impact_depth = 3;              // verify also runs tests of callers this deep (0 = off)
  int history_keep = 500;                  // lifecycle entries kept per branch (0 = all)
  std::string review_approval = "both";    // where unreviewed commits are queued: local | hub | both | off
  // [publish]: render into a git working copy, commit and push.
  std::string publish_git_dir;             // relative to the project root, or absolute
  std::string publish_remote;              // git remote name or URL ("" = commit only)
  std::string publish_branch = "main";     // git branch to publish to
  bool publish_on_push = false;            // hub: publish after every accepted push
};

class Repo {
 public:
  static std::optional<fs::path> find_root(fs::path start);
  static std::unique_ptr<Repo> init(const fs::path& root, const std::string& name);
  static std::unique_ptr<Repo> open(const fs::path& start);

  const fs::path& root() const { return root_; }
  fs::path hs_dir() const { return root_ / ".hs"; }
  fs::path scratch_dir() const { return hs_dir() / "scratch"; }
  Store& store() { return *store_; }
  const Config& config() const { return config_; }

  std::string branch() const;
  std::optional<Hash> head_commit() const;  // nullopt before the first commit
  Hash head_ns_hash();                      // namespace of HEAD (empty namespace if none)
  Hash work_ns_hash();
  Namespace work_ns();
  // Replaces the working namespace, refreshes the index, and records the
  // lifecycle step in the history (unless the state didn't change).
  void set_work_ns(const Namespace& ns, const std::string& step, const std::string& message = "");
  void set_work_ns_hash(const Hash& h, const std::string& step, const std::string& message = "");
  // Adds a history entry for the current branch.
  void record(const std::string& step, const Hash& ns, const std::string& commit,
              const std::string& message);

  // Who is acting: --as flag, HS_AGENT, config author, or user@host.
  std::string agent() const;
  void set_agent(const std::string& a) { agent_ = a; }

 private:
  Repo(fs::path root);
  void load_config();
  fs::path root_;
  std::unique_ptr<Store> store_;
  Config config_;
  std::string agent_;
};

Hash empty_namespace(Store& s);

}  // namespace hs
