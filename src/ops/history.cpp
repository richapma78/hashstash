#include "ops/history.h"

#include <set>
#include <stdexcept>

#include "ops/locks.h"

namespace hs {

CommitResult commit(Repo& repo, const std::string& message, bool keep_locks) {
  if (trim(message).empty())
    throw std::runtime_error("a commit needs a description of what changed (-m \"...\")");
  Store& s = repo.store();
  CommitResult res;
  Hash work = repo.work_ns_hash();
  Hash head_ns = repo.head_ns_hash();
  if (work == head_ns) throw std::runtime_error("nothing to commit");
  res.diff = diff_namespaces(s, load_namespace(s, head_ns), load_namespace(s, work));

  Commit c;
  c.ns = work;
  if (auto h = repo.head_commit()) c.parents.push_back(*h);
  c.author = repo.agent();
  c.time = now_seconds();
  c.message = trim(message);
  for (auto& d : res.diff.defs)
    c.changes.push_back(d.kind + " " + (d.new_name.empty() ? d.old_name : d.new_name));
  for (auto& f : res.diff.files_added) c.changes.push_back("new file " + f);
  for (auto& f : res.diff.files_removed) c.changes.push_back("deleted file " + f);
  for (auto& f : res.diff.files_glue_changed) c.changes.push_back("module code " + f);
  if (res.diff.tests_changed) c.changes.push_back("test links changed");
  if (res.diff.git_ignore_changed) c.changes.push_back("git-ignore rules changed");

  s.transaction([&] {
    res.commit = put_commit(s, c);
    s.set_ref("branch/" + repo.branch(), res.commit);
    repo.record("commit", work, res.commit, c.message);
    if (!keep_locks) {
      std::set<std::string> touched;
      for (auto& d : res.diff.defs) touched.insert(d.lineage);
      for (auto& l : list_locks(repo))
        if (l.holder == repo.agent() && touched.count(l.target)) res.locks_released += release_lock(repo, l.target);
    }
  });
  return res;
}

static std::vector<Hash> first_parents(Repo& repo, int limit) {
  std::vector<Hash> out;
  auto h = repo.head_commit();
  while (h && (limit <= 0 || (int)out.size() < limit)) {
    out.push_back(*h);
    Commit c = load_commit(repo.store(), *h);
    if (c.parents.empty()) break;
    h = c.parents[0];
  }
  return out;
}

nlohmann::json log(Repo& repo, int limit, const std::string& name) {
  Store& s = repo.store();
  nlohmann::json out = nlohmann::json::array();
  std::string lineage;
  if (!name.empty()) {
    Namespace w = repo.work_ns();
    for (auto& [q, id] : w.lineage)
      if (q == name) lineage = id;
    if (lineage.empty()) throw std::runtime_error("no definition " + name);
  }
  for (auto& h : first_parents(repo, name.empty() ? limit : 0)) {
    Commit c = load_commit(s, h);
    nlohmann::json j = {{"commit", short_hash(h)}, {"author", c.author}, {"time", format_time(c.time)},
                        {"message", c.message}};
    if (!lineage.empty()) {
      // Did this commit change the definition? Compare with its parent.
      Namespace now = load_namespace(s, c.ns);
      Namespace before = c.parents.empty() ? Namespace{} : load_namespace(s, load_commit(s, c.parents[0]).ns);
      auto d = diff_namespaces(s, before, now);
      bool hit = false;
      for (auto& dc : d.defs)
        if (dc.lineage == lineage) {
          j["change"] = dc.kind;
          j["name"] = dc.new_name.empty() ? dc.old_name : dc.new_name;
          hit = true;
        }
      if (!hit) continue;
    } else {
      j["changes"] = c.changes;
    }
    out.push_back(j);
    if (limit > 0 && (int)out.size() >= limit) break;
  }
  return out;
}

static HistoryEntry read_entry(Stmt& st) {
  return HistoryEntry{st.integer(0), st.integer(1), st.text(2), st.text(3), st.text(4), st.text(5), st.text(6)};
}

std::vector<HistoryEntry> history(Repo& repo, int limit) {
  Stmt st(repo.store().db(),
          "SELECT id, time, step, ns, commit_hash, agent, message FROM history WHERE branch = ? "
          "ORDER BY id DESC LIMIT ?");
  st.bind(1, repo.branch()).bind(2, (long long)(limit > 0 ? limit : 1000000));
  std::vector<HistoryEntry> out;
  while (st.step()) out.push_back(read_entry(st));
  return out;
}

std::optional<HistoryEntry> history_entry(Repo& repo, long long id) {
  Stmt st(repo.store().db(),
          "SELECT id, time, step, ns, commit_hash, agent, message FROM history WHERE id = ?");
  st.bind(1, id);
  if (!st.step()) return std::nullopt;
  return read_entry(st);
}

HistoryEntry rollback(Repo& repo, long long id) {
  auto e = history_entry(repo, id);
  if (!e) throw std::runtime_error("no history entry @" + std::to_string(id));
  if (!repo.store().has(e->ns)) throw std::runtime_error("history entry @" + std::to_string(id) + " was pruned");
  repo.set_work_ns_hash(e->ns, "rollback", "back to @" + std::to_string(id) + " (" + e->step + ")");
  return *e;
}

Hash resolve_ns(Repo& repo, const std::string& ref) {
  Store& s = repo.store();
  if (ref.empty() || ref == "work") return repo.work_ns_hash();
  if (ref == "head") return repo.head_ns_hash();
  if (ref[0] == '@') {
    auto e = history_entry(repo, std::stoll(ref.substr(1)));
    if (!e) throw std::runtime_error("no history entry " + ref);
    return e->ns;
  }
  if (starts_with(ref, "head~")) {
    int n = std::stoi(ref.substr(5));
    auto chain = first_parents(repo, n + 1);
    if ((int)chain.size() <= n) throw std::runtime_error("history is shorter than " + ref);
    return load_commit(s, chain[n]).ns;
  }
  auto h = s.expand(ref[0] == '#' ? ref.substr(1) : ref);
  if (!h) throw std::runtime_error("unknown reference '" + ref + "'");
  auto o = s.must_get(*h);
  if (o.kind == "commit") return load_commit(s, *h).ns;
  if (o.kind == "namespace") return *h;
  throw std::runtime_error(ref + " is not a commit");
}

}  // namespace hs
