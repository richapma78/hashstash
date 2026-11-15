#include "ops/repo.h"

#include <cstdlib>
#include <stdexcept>

#include "ops/index.h"

namespace hs {

Hash empty_namespace(Store& s) { return put_namespace(s, Namespace{}); }

Repo::Repo(fs::path root) : root_(std::move(root)) {
  store_ = std::make_unique<Store>(to_generic(hs_dir() / "store.db"));
  load_config();
}

std::optional<fs::path> Repo::find_root(fs::path start) {
  std::error_code ec;
  fs::path p = fs::absolute(start, ec);
  while (true) {
    if (fs::is_directory(p / ".hs", ec)) return p;
    if (!p.has_parent_path() || p.parent_path() == p) return std::nullopt;
    p = p.parent_path();
  }
}

static const char* kDefaultConfig = R"(# hashstash project settings
[project]
name = "%NAME%"

[user]
# author = "Your Name"

[size]
warn_lines = 60   # warn when a definition is longer than this
max_lines  = 0    # refuse longer definitions (0 = never refuse)

[locks]
minutes = 15      # a lock expires unless renewed within this time

[tests]
require = "warn"  # every function/method needs linked test cases: off | warn | block
# exempt = ["__repr__", "_*"]   # short names that need no tests
python = "python -m pytest -q {tests}"   # how hs verify runs Python tests
impact_depth = 3  # verify also runs the tests of callers up to this many levels away (0 = off)

[history]
keep = 500        # lifecycle steps kept per branch for rollback (0 = keep all)

[import]
# ignore = ["*.min.js", "data/**"]

[publish]
# Render each published branch into a git working copy, commit, and push.
# git_dir = "../myproject-git"   # created (git init / clone) if missing
# remote  = "origin"             # remote name or URL; empty = commit only
# branch  = "main"
# on_push = false                # hub: publish after every accepted push
)";

std::unique_ptr<Repo> Repo::init(const fs::path& root, const std::string& name) {
  if (fs::exists(root / ".hs")) throw std::runtime_error("already a hashstash project: " + to_generic(root));
  fs::create_directories(root / ".hs" / "scratch");
  std::string cfg = kDefaultConfig;
  cfg.replace(cfg.find("%NAME%"), 6, name);
  write_file(root / ".hs" / "config.toml", cfg);
  auto repo = std::unique_ptr<Repo>(new Repo(root));
  Store& s = repo->store();
  s.transaction([&] {
    Hash ns = empty_namespace(s);
    s.set_ref("HEAD", "main");
    s.set_ref("branch/main", "");
    s.set_ref("work/main", ns);
    s.set_ref("project/name", name);
  });
  return repo;
}

std::unique_ptr<Repo> Repo::open(const fs::path& start) {
  auto root = find_root(start);
  if (!root) throw std::runtime_error("not inside a hashstash project (run: hs init)");
  return std::unique_ptr<Repo>(new Repo(*root));
}

// A small TOML subset: [sections], key = "string" | number | ["a", "b"], # comments.
void Repo::load_config() {
  fs::path p = hs_dir() / "config.toml";
  if (!fs::exists(p)) return;
  std::string section;
  for (auto& raw : split_lines(read_file(p))) {
    std::string line = raw;
    bool in_str = false;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '"') in_str = !in_str;
      if (line[i] == '#' && !in_str) {
        line.resize(i);
        break;
      }
    }
    line = trim(line);
    if (line.empty()) continue;
    if (line.front() == '[') {
      section = trim(line.substr(1, line.find(']') - 1));
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = section + "." + trim(line.substr(0, eq));
    std::string val = trim(line.substr(eq + 1));
    auto unquote = [](std::string v) {
      v = trim(v);
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
      return v;
    };
    auto num = [&](int& out) {
      try {
        out = std::stoi(val);
      } catch (...) {
      }
    };
    if (key == "project.name") config_.project = unquote(val);
    else if (key == "user.author") config_.author = unquote(val);
    else if (key == "size.warn_lines") num(config_.warn_lines);
    else if (key == "size.max_lines") num(config_.max_lines);
    else if (key == "locks.minutes") num(config_.lock_minutes);
    else if (key == "tests.require") config_.tests_require = unquote(val);
    else if (key == "tests.python") config_.test_command_python = unquote(val);
    else if (key == "tests.impact_depth") num(config_.tests_impact_depth);
    else if (key == "history.keep") num(config_.history_keep);
    else if (key == "publish.git_dir") config_.publish_git_dir = unquote(val);
    else if (key == "publish.remote") config_.publish_remote = unquote(val);
    else if (key == "publish.branch") config_.publish_branch = unquote(val);
    else if (key == "publish.on_push") config_.publish_on_push = unquote(val) == "true";
    else if ((key == "import.ignore" || key == "tests.exempt") && val.size() >= 2 && val.front() == '[') {
      auto& list = key == "import.ignore" ? config_.ignore : config_.tests_exempt;
      std::string body = val.substr(1, val.rfind(']') - 1);
      size_t i = 0;
      while (i < body.size()) {
        size_t c = body.find(',', i);
        if (c == std::string::npos) c = body.size();
        std::string item = unquote(body.substr(i, c - i));
        if (!item.empty()) list.push_back(item);
        i = c + 1;
      }
    }
  }
}

std::string Repo::branch() const { return store_->ref("HEAD").value_or("main"); }

std::optional<Hash> Repo::head_commit() const {
  auto h = store_->ref("branch/" + branch());
  if (!h || h->empty()) return std::nullopt;
  return h;
}

Hash Repo::head_ns_hash() {
  auto c = head_commit();
  if (!c) return empty_namespace(*store_);
  return load_commit(*store_, *c).ns;
}

Hash Repo::work_ns_hash() {
  auto h = store_->ref("work/" + branch());
  if (!h || h->empty()) return head_ns_hash();
  return *h;
}

Namespace Repo::work_ns() { return load_namespace(*store_, work_ns_hash()); }

void Repo::set_work_ns(const Namespace& ns, const std::string& step, const std::string& message) {
  set_work_ns_hash(put_namespace(*store_, ns), step, message);
}

void Repo::set_work_ns_hash(const Hash& h, const std::string& step, const std::string& message) {
  store_->transaction([&] {
    bool changed = work_ns_hash() != h;
    store_->set_ref("work/" + branch(), h);
    rebuild_index(*this);
    if (changed) record(step, h, "", message);
  });
}

void Repo::record(const std::string& step, const Hash& ns, const std::string& commit,
                  const std::string& message) {
  Stmt st(store_->db(),
          "INSERT INTO history(branch, time, step, ns, commit_hash, agent, message) VALUES (?, ?, ?, ?, ?, ?, ?)");
  st.bind(1, branch()).bind(2, now_seconds()).bind(3, step).bind(4, ns).bind(5, commit)
      .bind(6, agent()).bind(7, message).run();
  if (config_.history_keep > 0) {
    Stmt prune(store_->db(),
               "DELETE FROM history WHERE branch = ? AND id <= "
               "(SELECT id FROM history WHERE branch = ? ORDER BY id DESC LIMIT 1 OFFSET ?)");
    prune.bind(1, branch()).bind(2, branch()).bind(3, (long long)config_.history_keep).run();
  }
}

std::string Repo::agent() const {
  if (!agent_.empty()) return agent_;
  if (const char* a = std::getenv("HS_AGENT"); a && *a) return a;
  if (!config_.author.empty()) return config_.author;
  const char* user = std::getenv("USERNAME");
  if (!user) user = std::getenv("USER");
  const char* host = std::getenv("COMPUTERNAME");
  if (!host) host = std::getenv("HOSTNAME");
  return std::string(user ? user : "someone") + "@" + (host ? host : "localhost");
}

}  // namespace hs
