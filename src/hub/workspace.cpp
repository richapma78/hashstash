#include "hub/workspace.h"

#include <deque>
#include <stdexcept>
#include <unordered_set>

#include "ops/review.h"
#include "ops/tests.h"

namespace hs {

using json = nlohmann::json;

Workspace::Workspace(std::shared_ptr<ProjectStore> p, const fs::path& cache_dir, bool force_cache)
    : p_(std::move(p)), dir_(cache_dir) {
  native_ = force_cache ? nullptr : p_->native_repo();
  if (native_) {
    repo_ = native_;
  } else {
    if (dir_.empty()) throw std::runtime_error("project " + p_->name() + " needs a workspace cache folder on this hub");
    open_cache();
  }
  // Locks, work items and requests made through this workspace go to the
  // backend, the one authority all hub servers share.
  repo_->set_coordination_override(&p_->coord());
  repo_->set_agent("hub");
}

void Workspace::open_cache() {
  std::string cfg = p_->config_text();
  fs::path root = dir_ / from_utf8(p_->name());
  if (!fs::exists(root / ".hs")) {
    fs::create_directories(root);
    Repo::init(root, p_->name());
  }
  if (!cfg.empty() && read_file(root / ".hs" / "config.toml") != cfg) write_file(root / ".hs" / "config.toml", cfg);
  std::string agent = owned_ ? owned_->agent() : "hub";
  owned_ = Repo::open(root);
  owned_->set_agent(agent);
  owned_->set_coordination_override(&p_->coord());
  repo_ = owned_.get();
}

void Workspace::fetch(const Hash& root) {
  if (native_ || root.empty()) return;
  Store& s = repo_->store();
  std::deque<Hash> q{root};
  std::unordered_set<Hash> seen;
  s.transaction([&] {
    while (!q.empty()) {
      Hash h = q.front();
      q.pop_front();
      if (!seen.insert(h).second || s.has(h)) continue;  // what we have, we have completely
      auto o = p_->get(h);
      if (!o) throw std::runtime_error("backend is missing object #" + short_hash(h));
      if (s.put(o->kind, o->body) != h) throw std::runtime_error("backend object #" + short_hash(h) + " is corrupt");
      for (auto& r : object_refs(*o)) q.push_back(r);
    }
  });
}

// When the branch head moved (a push, or a commit through the hub's tools),
// the workspace follows it: working state, index, and test results.
void Workspace::follow_heads() {
  Store& s = repo_->store();
  std::string b = repo_->branch();
  auto heads = p_->branches();
  auto it = heads.find(b);
  if (it == heads.end() || it->second.empty()) return;
  Hash head = it->second;
  if (s.ref("hub/seen/" + b).value_or("") == head) return;
  Commit c = load_commit(s, head);
  repo_->set_work_ns_hash(c.ns, "push", c.author + " pushed #" + short_hash(head) + ": " + c.message);
  std::string report = p_->verification_report(c.ns);
  if (!report.empty()) {
    if (!native_) {
      Stmt st(s.db(), "INSERT OR REPLACE INTO verification(ns, status, time, agent, report) VALUES (?, ?, ?, 'hub', ?)");
      st.bind(1, c.ns).bind(2, p_->verification(c.ns)).bind(3, now_seconds()).bind(4, report).run();
    }
    try {
      record_test_results(s, json::parse(report));
    } catch (...) {
    }
  }
  s.set_ref("hub/seen/" + b, head);
}

void Workspace::refresh() {
  if (native_) {
    follow_heads();
    return;
  }
  std::string cfg = p_->config_text();
  if (!cfg.empty() && read_file(repo_->hs_dir() / "config.toml") != cfg) open_cache();
  Store& s = repo_->store();
  for (auto& [b, head] : p_->branches()) {
    if (s.ref("branch/" + b).value_or("") == head) continue;
    fetch(head);
    s.set_ref("branch/" + b, head);
  }
  std::string b = repo_->branch();
  std::string bw = p_->ref("work/" + b);
  if (!bw.empty() && bw != s.ref("hub/seen-work/" + b).value_or("")) {
    fetch(bw);
    repo_->set_work_ns_hash(bw, "sync", "hub working state");
    s.set_ref("hub/seen-work/" + b, bw);
  }
  follow_heads();
  // Reviews and the push log, for the review pages.
  long long seq = 0;
  try {
    seq = std::stoll(s.ref("hub/review_seq").value_or("0"));
  } catch (...) {
  }
  for (;;) {
    json res = p_->reviews_after(seq);
    for (auto& r : res["reviews"]) store_review(s, review_from_json(r), true);
    long long last = res.value("last", seq);
    if (last == seq || res["reviews"].empty()) break;
    seq = last;
  }
  s.set_ref("hub/review_seq", std::to_string(seq));
  long long pid = 0;
  try {
    pid = std::stoll(s.ref("hub/push_seq").value_or("0"));
  } catch (...) {
  }
  for (;;) {
    json res = p_->pushes_after(pid);
    Stmt st(s.db(), "INSERT OR IGNORE INTO push_log(id, branch, old_head, new_head, agent, time) VALUES (?, ?, ?, ?, ?, ?)");
    for (auto& x : res["pushes"]) {
      st.bind(1, x.value("id", 0LL)).bind(2, x.value("branch", "")).bind(3, x.value("old", ""))
          .bind(4, x.value("new", "")).bind(5, x.value("agent", "")).bind(6, x.value("time", 0LL)).run();
      st.reset();
    }
    long long last = res.value("last", pid);
    if (last == pid || res["pushes"].empty()) break;
    pid = last;
  }
  s.set_ref("hub/push_seq", std::to_string(pid));
}

void Workspace::save(const std::string& agent) {
  if (native_) return;
  Store& s = repo_->store();
  auto upload = [&](const Hash& root) {
    if (root.empty()) return;
    std::deque<Hash> q{root};
    std::unordered_set<Hash> seen;
    p_->begin();
    try {
      while (!q.empty()) {
        Hash h = q.front();
        q.pop_front();
        if (!seen.insert(h).second || p_->has(h)) continue;
        Object o = s.must_get(h);
        p_->put(h, o);
        for (auto& r : object_refs(o)) q.push_back(r);
      }
      p_->commit();
    } catch (...) {
      p_->rollback();
      throw;
    }
  };
  auto ancestors = [&](const Hash& head) {
    std::unordered_set<Hash> out;
    std::deque<Hash> q;
    if (!head.empty()) q.push_back(head);
    while (!q.empty()) {
      Hash h = q.front();
      q.pop_front();
      if (!out.insert(h).second) continue;
      for (auto& par : load_commit(s, h).parents) q.push_back(par);
    }
    return out;
  };
  auto heads = p_->branches();
  for (auto& [ref, mine] : s.refs_with_prefix("branch/")) {
    std::string b = ref.substr(7);
    std::string theirs = heads.count(b) ? heads[b] : "";
    if (mine.empty() || mine == theirs) continue;
    auto anc = ancestors(mine);
    if (!theirs.empty() && !anc.count(theirs))
      throw std::runtime_error("the hub's " + b + " moved while this was being changed; retry");
    upload(mine);
    Hash ns = load_commit(s, mine).ns;
    {
      Stmt st(s.db(), "SELECT status, report FROM verification WHERE ns = ?");
      st.bind(1, ns);
      if (st.step()) p_->set_verification(ns, st.text(0), st.text(1));
    }
    if (!p_->cas_branch(b, theirs, mine))
      throw std::runtime_error("the hub's " + b + " moved while this was being changed; retry");
    p_->log_push(b, theirs, mine, agent);
    auto before = ancestors(theirs);
    std::vector<std::string> landed;
    for (auto& c : anc)
      if (!before.count(c)) landed.push_back(c);
    p_->coord().land(landed, mine);
    s.set_ref("hub/seen/" + b, mine);
  }
  std::string b = repo_->branch();
  std::string seen_work = s.ref("hub/seen-work/" + b).value_or("");
  Hash work = repo_->work_ns_hash();
  if (work != seen_work) {
    upload(work);
    if (!p_->cas_ref("work/" + b, seen_work, work))
      throw std::runtime_error("the hub's working state changed while this was being changed; retry");
    s.set_ref("hub/seen-work/" + b, work);
  }
  auto rs = unsynced_reviews(s);
  for (auto& r : rs) p_->put_review(review_to_json(r));
  if (!rs.empty()) mark_reviews_synced(s, rs);
}

}  // namespace hs
