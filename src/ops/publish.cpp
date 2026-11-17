#include "ops/publish.h"

#include <stdexcept>

#include "ops/import_render.h"

namespace hs {

std::string normalize_git_ignore(const std::string& in) {
  std::string p = trim(in);
  for (char& c : p)
    if (c == '\\') c = '/';
  while (starts_with(p, "./")) p = p.substr(2);
  while (!p.empty() && p.front() == '/') p.erase(0, 1);
  while (!p.empty() && p.back() == '/') p.pop_back();
  if (p.empty()) throw std::runtime_error("a git-ignore rule needs a path or pattern, e.g. secrets/ or *.env");
  return p;
}

bool git_ignore_matches(const std::string& p, const std::string& path) {
  if (glob_match(p, path) || glob_match(p + "/**", path)) return true;
  // No slash: a name at any depth, as in .gitignore.
  return p.find('/') == std::string::npos && (glob_match("**/" + p, path) || glob_match("**/" + p + "/**", path));
}

bool git_ignored(const Namespace& ns, const std::string& path) {
  for (auto& [p, why] : ns.git_ignore)
    if (git_ignore_matches(p, path)) return true;
  return false;
}

static nlohmann::json rule_json(const Namespace& ns, const std::string& p, const std::string& why) {
  nlohmann::json files = nlohmann::json::array();
  for (auto& [path, e] : ns.files)
    if (git_ignore_matches(p, path)) files.push_back(path);
  return {{"pattern", p}, {"reason", why}, {"files", files}};
}

nlohmann::json git_ignore_list(Repo& repo) {
  Namespace ns = repo.work_ns();
  nlohmann::json out = nlohmann::json::array();
  for (auto& [p, why] : ns.git_ignore) out.push_back(rule_json(ns, p, why));
  return out;
}

nlohmann::json git_ignore_add(Repo& repo, const std::string& pattern, const std::string& reason) {
  if (trim(reason).empty())
    throw std::runtime_error("say why it stays out of git (--reason), so other agents know");
  std::string p = normalize_git_ignore(pattern);
  Namespace ns = repo.work_ns();
  ns.git_ignore[p] = trim(reason);
  repo.set_work_ns(ns, "git-ignore", "git-ignore " + p);
  return rule_json(ns, p, trim(reason));
}

void git_ignore_remove(Repo& repo, const std::string& pattern) {
  std::string p = normalize_git_ignore(pattern);
  Namespace ns = repo.work_ns();
  if (!ns.git_ignore.erase(p)) throw std::runtime_error("no git-ignore rule " + p);
  repo.set_work_ns(ns, "git-ignore", "stop git-ignoring " + p);
}

bool publish_on_push(const Repo& repo) {
  return repo.config().publish_on_push && !repo.config().publish_git_dir.empty();
}

static RunResult git(const fs::path& dir, const std::string& args) {
  // Commit the rendered bytes exactly; never let a global autocrlf rewrite them.
  return run_command(dir, "git -c core.autocrlf=false -c core.safecrlf=false " + args);
}

static void must(const RunResult& r, const std::string& what) {
  if (r.code != 0) throw std::runtime_error(what + " failed: " + trim(r.output));
}

nlohmann::json publish(Repo& repo, const std::string& branch_in) {
  const Config& c = repo.config();
  if (c.publish_git_dir.empty()) throw std::runtime_error("no [publish] git_dir in .hs/config.toml");
  std::string branch = branch_in.empty() ? repo.branch() : branch_in;
  Store& s = repo.store();
  auto head = s.ref("branch/" + branch);
  if (!head || head->empty()) throw std::runtime_error("branch " + branch + " has no commits");

  fs::path dir = from_utf8(c.publish_git_dir);
  if (dir.is_relative()) dir = repo.root() / dir;
  bool remote_is_url = c.publish_remote.find("://") != std::string::npos ||
                       c.publish_remote.find('@') != std::string::npos ||
                       fs::exists(from_utf8(c.publish_remote));
  std::string remote = remote_is_url ? "origin" : c.publish_remote;

  // Prepare the working copy.
  if (!fs::exists(dir / ".git")) {
    fs::create_directories(dir);
    if (remote_is_url) must(git(dir.parent_path(), "clone " + shell_quote(c.publish_remote) + " " + shell_quote(dir.string())), "git clone");
    else must(git(dir, "init -q"), "git init");
  }
  if (!remote.empty()) {
    git(dir, "fetch -q " + remote);
    // Follow the remote branch if it exists, so the publish is a fast-forward.
    if (git(dir, "rev-parse --verify -q " + remote + "/" + c.publish_branch).code == 0)
      must(git(dir, "checkout -q -B " + c.publish_branch + " " + remote + "/" + c.publish_branch), "git checkout");
    else git(dir, "checkout -q -B " + c.publish_branch);
  } else {
    git(dir, "checkout -q -B " + c.publish_branch);
  }

  Hash ns = load_commit(s, *head).ns;
  Namespace tree = load_namespace(s, ns);
  RenderResult rr = render_to(repo, dir, ns, [&](const std::string& path) { return git_ignored(tree, path); });

  // Commit message: the hashstash commits since the last publish, by id and message. Authors are left out: they are
  // <OS user>@<computer> by default, and a published repository may be public.
  std::string last = s.ref("publish/" + branch).value_or("");
  std::string body;
  int count = 0;
  for (auto h = *head; !h.empty() && h != last && count < 50;) {
    Commit cm = load_commit(s, h);
    body += "- " + cm.message + " (#" + short_hash(h) + ")\n";
    ++count;
    h = cm.parents.empty() ? "" : cm.parents[0];
  }
  std::string message = "hashstash publish #" + short_hash(*head) + " (" + branch + ")\n\n" + body;
  write_file(repo.hs_dir() / "publish-message.txt", message);

  must(git(dir, "add -A"), "git add");
  bool changed = git(dir, "diff --cached --quiet").code != 0;
  // Every publish of a new hashstash commit is recorded on the git branch, even when the files are already
  // identical (a branch adopted from an existing git history, or a commit that only changed hashstash-side
  // data): an empty commit names the hashstash commit, so git always says which commit it holds.
  std::string tip = trim(git(dir, "log -1 --format=%s").output);
  bool recorded = tip.rfind("hashstash publish #" + short_hash(*head) + " ", 0) == 0;
  bool record = changed || !recorded;
  nlohmann::json out = {{"commit", short_hash(*head)}, {"dir", to_generic(dir)}, {"written", rr.written},
                        {"deleted", rr.deleted}, {"git_ignored", rr.ignored}, {"changed", changed}};
  if (record) {
    must(git(dir, std::string("-c user.name=hashstash -c user.email=hashstash@localhost commit -q ") +
                      (changed ? "" : "--allow-empty ") + "-F " +
                      shell_quote((repo.hs_dir() / "publish-message.txt").string())),
         "git commit");
    out["git_commit"] = trim(git(dir, "rev-parse --short HEAD").output);
  }
  // Push when this publish committed, or when the remote branch is missing or behind (an earlier push failed).
  bool behind = !remote.empty() &&
                (git(dir, "rev-parse --verify -q " + remote + "/" + c.publish_branch).code != 0 ||
                 trim(git(dir, "rev-list --count " + remote + "/" + c.publish_branch + "..HEAD").output) != "0");
  if (!remote.empty() && (record || behind)) {
    must(git(dir, "push -q " + remote + " " + c.publish_branch), "git push");
    out["pushed"] = remote + "/" + c.publish_branch;
  }
  s.set_ref("publish/" + branch, *head);
  repo.record("publish", ns, *head, changed ? "published to " + to_generic(dir) : "publish: no changes");
  return out;
}

}  // namespace hs
