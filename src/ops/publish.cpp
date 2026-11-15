#include "ops/publish.h"

#include <stdexcept>

#include "ops/import_render.h"

namespace hs {

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
  RenderResult rr = render_to(repo, dir, ns);

  // Commit message: the hashstash commits since the last publish.
  std::string last = s.ref("publish/" + branch).value_or("");
  std::string body;
  int count = 0;
  for (auto h = *head; !h.empty() && h != last && count < 50;) {
    Commit cm = load_commit(s, h);
    body += "- " + cm.message + " (#" + short_hash(h) + ", " + cm.author + ")\n";
    ++count;
    h = cm.parents.empty() ? "" : cm.parents[0];
  }
  std::string message = "hashstash publish #" + short_hash(*head) + " (" + branch + ")\n\n" + body;
  write_file(repo.hs_dir() / "publish-message.txt", message);

  must(git(dir, "add -A"), "git add");
  bool changed = git(dir, "diff --cached --quiet").code != 0;
  nlohmann::json out = {{"commit", short_hash(*head)}, {"dir", to_generic(dir)}, {"written", rr.written},
                        {"deleted", rr.deleted}, {"changed", changed}};
  if (changed) {
    must(git(dir, "-c user.name=hashstash -c user.email=hashstash@localhost commit -q -F " +
                      shell_quote((repo.hs_dir() / "publish-message.txt").string())),
         "git commit");
    out["git_commit"] = trim(git(dir, "rev-parse --short HEAD").output);
  }
  if (!remote.empty() && changed) {
    must(git(dir, "push -q " + remote + " " + c.publish_branch), "git push");
    out["pushed"] = remote + "/" + c.publish_branch;
  }
  s.set_ref("publish/" + branch, *head);
  repo.record("publish", ns, *head, changed ? "published to " + to_generic(dir) : "publish: no changes");
  return out;
}

}  // namespace hs
