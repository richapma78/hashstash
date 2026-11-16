#include "ops/sync.h"

#include "ops/coord.h"

#include <httplib.h>

#include <deque>
#include <set>
#include <stdexcept>
#include <unordered_set>

#include "ops/locks.h"
#include "ops/review.h"
#include "ops/tests.h"
#include "ops/verify.h"

namespace hs {

using json = nlohmann::json;

namespace {

class HttpTransport : public HubTransport {
 public:
  explicit HttpTransport(const std::string& url) {
    // http://host:port/project
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) throw std::runtime_error("remote URL must look like http://host:port/project");
    size_t slash = url.find('/', scheme + 3);
    if (slash == std::string::npos || slash + 1 >= url.size())
      throw std::runtime_error("remote URL has no project: " + url);
    base_ = url.substr(0, slash);
    project_ = url.substr(slash + 1);
    while (!project_.empty() && project_.back() == '/') project_.pop_back();
    client_ = std::make_unique<httplib::Client>(base_);
    client_->set_connection_timeout(10);
    client_->set_read_timeout(600);
    client_->set_write_timeout(600);
    if (const char* t = std::getenv("HS_TOKEN"); t && *t) client_->set_bearer_token_auth(t);
  }
  json call(const std::string& op, const json& req) override {
    httplib::Headers headers;
    if (std::string k = access_key(); !k.empty()) headers.emplace("X-HS-Key", k);
    auto res = client_->Post("/api/p/" + project_ + "/" + op, headers, req.dump(), "application/json");
    if (!res) throw std::runtime_error("hub " + base_ + " unreachable (" + httplib::to_string(res.error()) + ")");
    json body;
    try {
      body = json::parse(res->body);
    } catch (...) {
      throw std::runtime_error("hub returned " + std::to_string(res->status) + ": " + res->body.substr(0, 200));
    }
    if (res->status != 200) throw std::runtime_error(body.value("error", "hub error " + std::to_string(res->status)));
    return body;
  }

 private:
  std::string base_, project_;
  std::unique_ptr<httplib::Client> client_;
};

HubTransport* g_lock_override = nullptr;

std::vector<Hash> local_reachable(const Store& s, const Hash& root, const std::unordered_set<Hash>& stop) {
  std::vector<Hash> out;
  std::unordered_set<Hash> seen;
  std::deque<Hash> q{root};
  while (!q.empty()) {
    Hash h = q.front();
    q.pop_front();
    if (!seen.insert(h).second || stop.count(h)) continue;
    auto o = s.get(h);
    if (!o) continue;
    out.push_back(h);
    for (auto& r : object_refs(*o)) q.push_back(r);
  }
  return out;
}

std::unordered_set<Hash> local_ancestors(const Store& s, const Hash& head) {
  std::unordered_set<Hash> out;
  std::deque<Hash> q;
  if (!head.empty()) q.push_back(head);
  while (!q.empty()) {
    Hash h = q.front();
    q.pop_front();
    if (!out.insert(h).second) continue;
    auto o = s.get(h);
    if (!o || o->kind != "commit") continue;
    for (auto& p : load_commit(s, h).parents) q.push_back(p);
  }
  return out;
}

// Commits reachable from head that are not ancestors of base.
std::vector<Hash> local_reachable_commits(const Store& s, const Hash& head, const Hash& base) {
  auto stop = local_ancestors(s, base);
  std::vector<Hash> out;
  for (auto& h : local_ancestors(s, head))
    if (!stop.count(h)) out.push_back(h);
  return out;
}

void sync_locks_down(Repo& repo, const std::string& remote, const json& locks) {
  Store& s = repo.store();
  s.transaction([&] {
    Stmt del(s.db(), "DELETE FROM lock WHERE origin = ?");
    del.bind(1, remote).run();
    Stmt ins(s.db(),
             "INSERT OR REPLACE INTO lock(target, name, holder, acquired, expires, origin, work) VALUES (?, ?, ?, ?, ?, ?, ?)");
    // Ours that we don't have yet: handed to us by the hub (a queued request).
    Stmt own(s.db(),
             "INSERT OR IGNORE INTO lock(target, name, holder, acquired, expires, origin, work) VALUES (?, ?, ?, ?, ?, '', ?)");
    for (auto& l : locks) {
      Stmt& st = l.at("holder") == repo.agent() ? own : ins;
      st.bind(1, l.at("target").get<std::string>()).bind(2, l.at("name").get<std::string>())
          .bind(3, l.at("holder").get<std::string>()).bind(4, now_seconds())
          .bind(5, l.at("expires").get<long long>());
      if (&st == &ins) ins.bind(6, remote).bind(7, l.value("work", ""));
      else own.bind(6, l.value("work", ""));
      st.run();
      st.reset();
    }
  });
}

// Takes/releases locks on the hub when a remote "origin" is configured.
void remote_lock_hook(Repo& repo, const LockInfo& l, bool acquire, bool force) {
  if (const char* off = std::getenv("HS_OFFLINE"); off && *off) return;
  std::unique_ptr<HubTransport> owned;
  HubTransport* t = g_lock_override;
  if (!t) {
    if (!remote_url(repo, "origin")) return;
    owned = open_remote(repo, "origin");
    t = owned.get();
  }
  if (acquire) {
    json r = t->call("lock", {{"target", l.target}, {"name", l.name}, {"agent", l.holder}, {"work", l.work},
                              {"minutes", repo.config().lock_minutes}});
    if (!r.value("ok", false)) {
      LockRec b = LockRec::from_json(r["held_by"]);
      throw LockedError(l.name, LockInfo{b.target, b.name, b.holder, b.work, b.acquired, b.expires});
    }
  } else {
    t->call("unlock", {{"target", l.target}, {"agent", l.holder}, {"force", force}});
  }
}

struct RegisterHook {
  RegisterHook() { set_remote_lock_hook(remote_lock_hook); }
} register_hook;

}  // namespace

std::unique_ptr<HubTransport> http_transport(const std::string& url) { return std::make_unique<HttpTransport>(url); }

int send_reviews(Repo& repo, HubTransport& hub) {
  auto rs = unsynced_reviews(repo.store());
  if (rs.empty()) return 0;
  json arr = json::array();
  for (auto& r : rs) arr.push_back(review_to_json(r));
  hub.call("put_reviews", {{"reviews", arr}});
  mark_reviews_synced(repo.store(), rs);
  return (int)rs.size();
}

int fetch_reviews(Repo& repo, HubTransport& hub, const std::string& remote) {
  Store& s = repo.store();
  std::string key = "remote/" + remote + "/review_seq";
  long long seq = 0;
  try {
    seq = std::stoll(s.ref(key).value_or("0"));
  } catch (...) {
  }
  int got = 0;
  for (;;) {
    json res = hub.call("reviews", {{"after", seq}});
    for (auto& r : res["reviews"])
      if (store_review(s, review_from_json(r), true)) ++got;
    long long last = res.value("last", seq);
    if (last == seq || res["reviews"].empty()) break;
    seq = last;
  }
  s.set_ref(key, std::to_string(seq));
  return got;
}

std::string share_reviews(Repo& repo) {
  if (const char* off = std::getenv("HS_OFFLINE"); off && *off) return "";
  if (!remote_url(repo, "origin")) return "";
  try {
    auto hub = open_remote(repo, "origin");
    send_reviews(repo, *hub);
    return "";
  } catch (std::exception& e) {
    return std::string("review saved here; not sent to the hub yet (") + e.what() + "); hs push sends it later";
  }
}

void set_lock_transport_override(HubTransport* t) { g_lock_override = t; }
HubTransport* lock_transport_override() { return g_lock_override; }

static std::string g_access_key;
void set_access_key(const std::string& key) { g_access_key = key; }
std::string access_key() {
  if (!g_access_key.empty()) return g_access_key;
  for (const char* v : {"HS_WORK_KEY", "HS_ORCH_KEY"})
    if (const char* k = std::getenv(v); k && *k) return k;
  return "";
}

void set_remote(Repo& repo, const std::string& name, const std::string& url) {
  repo.store().set_ref("remote/" + name, url);
}

std::optional<std::string> remote_url(Repo& repo, const std::string& name) {
  auto u = repo.store().ref("remote/" + name);
  if (!u || u->empty()) return std::nullopt;
  return u;
}

std::unique_ptr<HubTransport> open_remote(Repo& repo, const std::string& name) {
  auto u = remote_url(repo, name);
  if (!u) throw std::runtime_error("no remote '" + name + "' (hs remote add " + name + " <url>)");
  return http_transport(*u);
}

json push(Repo& repo, HubTransport& hub, const std::string& remote, const std::string& branch_in) {
  Store& s = repo.store();
  std::string branch = branch_in.empty() ? repo.branch() : branch_in;
  auto head = s.ref("branch/" + branch);
  if (!head || head->empty()) throw std::runtime_error("nothing to push: " + branch + " has no commits");
  Hash ns = load_commit(s, *head).ns;

  // Verification gate (checked here for a clear message; the hub enforces it).
  std::string report = "{}", status;
  {
    Stmt st(s.db(), "SELECT status, report FROM verification WHERE ns = ?");
    st.bind(1, ns);
    if (st.step()) {
      status = st.text(0);
      report = st.text(1);
    }
  }
  if (status != "passed")
    throw std::runtime_error("head #" + short_hash(*head) + " is " + (status.empty() ? "not verified" : "failing verification") +
                             ": run hs verify (it must pass) before pushing");

  json refs = hub.call("refs", json::object());
  std::string theirs = refs["branches"].value(branch, "");
  if (theirs == *head) {
    int sent = send_reviews(repo, hub);
    return {{"branch", branch}, {"head", *head}, {"uploaded", 0}, {"reviews_sent", sent},
            {"note", sent ? "already up to date; sent " + std::to_string(sent) + " review(s)" : "already up to date"}};
  }
  auto mine = local_ancestors(s, *head);
  if (!theirs.empty() && !mine.count(theirs))
    throw std::runtime_error("the hub's " + branch + " has commits you don't have: pull first");

  // Objects the hub lacks: everything reachable from our head, minus the
  // history it already has, filtered by asking.
  auto known = local_ancestors(s, theirs);
  auto candidates = local_reachable(s, *head, known);
  std::vector<Hash> missing;
  for (size_t i = 0; i < candidates.size(); i += 2000) {
    json batch(std::vector<Hash>(candidates.begin() + i, candidates.begin() + std::min(candidates.size(), i + 2000)));
    json res = hub.call("missing", {{"hashes", batch}});
    for (auto& h : res["missing"]) missing.push_back(h);
  }
  size_t uploaded = 0;
  json objs = json::array();
  size_t bytes = 0;
  auto flush = [&] {
    if (objs.empty()) return;
    hub.call("put_objects", {{"objects", objs}});
    uploaded += objs.size();
    objs = json::array();
    bytes = 0;
  };
  for (auto& h : missing) {
    Object o = s.must_get(h);
    bytes += o.body.size();
    objs.push_back({{"h", h}, {"k", o.kind}, {"b", base64_encode(o.body)}});
    if (bytes > 4 * 1024 * 1024 || objs.size() >= 2000) flush();
  }
  flush();

  json locks = json::array();
  for (auto& l : list_locks(repo))
    if (l.holder == repo.agent())
      locks.push_back({{"target", l.target}, {"name", l.name}, {"work", l.work}, {"expires", l.expires}});
  json verification = {{"status", status}};
  try {
    verification["report"] = json::parse(report);
  } catch (...) {
  }
  json res = hub.call("push", {{"branch", branch}, {"old", theirs}, {"new", *head}, {"agent", repo.agent()},
                               {"verification", verification}, {"locks", locks}});
  s.set_ref("remote/" + remote + "/" + branch, *head);
  repo.record("push", ns, *head, "pushed " + branch + " to " + remote);
  log_push(s, branch, theirs, *head, repo.agent());
  int sent = send_reviews(repo, hub);
  // How much of what was just pushed a person has looked at.
  int commits = 0, human = 0, ai = 0;
  for (auto& c : local_reachable_commits(s, *head, theirs)) {
    ++commits;
    auto st = review_status(s, c);
    if (st.human_reviewed) ++human;
    else if (st.ai_reviewed) ++ai;
  }
  (void)res;
  return {{"branch", branch}, {"head", *head}, {"uploaded", uploaded}, {"reviews_sent", sent},
          {"commits", commits}, {"human_reviewed", human}, {"ai_reviewed_only", ai},
          {"unreviewed", commits - human - ai}};
}

json pull(Repo& repo, HubTransport& hub, const std::string& remote, const std::string& branch_in) {
  Store& s = repo.store();
  std::string branch = branch_in.empty() ? repo.branch() : branch_in;
  json refs = hub.call("refs", json::object());
  sync_locks_down(repo, remote, refs["locks"]);
  std::string theirs = refs["branches"].value(branch, "");
  json out = {{"branch", branch}, {"locks", refs["locks"].size()}};
  send_reviews(repo, hub);
  out["reviews_fetched"] = fetch_reviews(repo, hub, remote);
  if (theirs.empty()) {
    out["note"] = "the hub has no " + branch + " branch yet";
    return out;
  }

  // Fetch what we lack.
  json have = json::array();
  {
    Stmt st(s.db(), "SELECT hash FROM object WHERE kind = 'commit'");
    while (st.step()) have.push_back(st.text(0));
  }
  json reach = hub.call("reachable", {{"from", theirs}, {"have", have}})["hashes"];
  std::vector<Hash> want;
  for (auto& h : reach)
    if (!s.has(h)) want.push_back(h);
  s.transaction([&] {
    for (size_t i = 0; i < want.size(); i += 1000) {
      json batch(std::vector<Hash>(want.begin() + i, want.begin() + std::min(want.size(), i + 1000)));
      json got = hub.call("get_objects", {{"hashes", batch}});
      for (auto& o : got["objects"]) {
        Object obj{o.at("k"), base64_decode(o.at("b").get<std::string>())};
        if (hash_bytes(obj.kind, obj.body) != o.at("h").get<std::string>()) throw std::runtime_error("hub sent a corrupt object");
        s.put(obj.kind, obj.body);
      }
    }
  });
  out["fetched"] = want.size();
  s.set_ref("remote/" + remote + "/" + branch, theirs);

  // Integrate: fast-forward when possible.
  std::string mine = s.ref("branch/" + branch).value_or("");
  if (mine == theirs) {
    out["result"] = "up to date";
    return out;
  }
  auto theirs_anc = local_ancestors(s, theirs);
  if (!mine.empty() && local_ancestors(s, mine).count(theirs)) {
    out["result"] = "ahead of the hub (push to share)";
    return out;
  }
  if (!mine.empty() && !theirs_anc.count(mine))
    throw std::runtime_error("your " + branch + " and the hub's have diverged; merging is not supported yet");
  bool on_branch = repo.branch() == branch;
  Hash old_ns = mine.empty() ? empty_namespace(s) : load_commit(s, mine).ns;
  if (on_branch && repo.work_ns_hash() != old_ns)
    throw std::runtime_error("you have uncommitted changes: commit them, then pull");
  Hash new_ns = load_commit(s, theirs).ns;
  s.set_ref("branch/" + branch, theirs);
  if (on_branch) repo.set_work_ns_hash(new_ns, "pull", "fast-forward to #" + short_hash(theirs) + " from " + remote);
  // The hub verified each commit before accepting it; the reports carry the
  // test results, so summaries here match the hub's. Oldest first, so the
  // newest result for a test wins.
  {
    auto mine_anc = mine.empty() ? std::unordered_set<Hash>{} : local_ancestors(s, mine);
    std::vector<std::pair<long long, Hash>> fresh;
    for (auto& c : theirs_anc)
      if (!mine_anc.count(c)) {
        Commit cc = load_commit(s, c);
        fresh.push_back({cc.time, cc.ns});
      }
    std::sort(fresh.begin(), fresh.end());
    Stmt st(s.db(), "INSERT OR IGNORE INTO verification(ns, status, time, agent, report) VALUES (?, 'passed', ?, ?, ?)");
    for (size_t i = 0; i < fresh.size(); i += 200) {
      json batch = json::array();
      for (size_t j = i; j < std::min(fresh.size(), i + 200); ++j) batch.push_back(fresh[j].second);
      json reports;
      try {
        reports = hub.call("verification", {{"ns", batch}})["reports"];
      } catch (...) {
        reports = json::array();  // an older hub: results stay local
      }
      std::map<std::string, json> by_ns;
      for (auto& r : reports) by_ns[r.value("ns", "")] = r.value("report", json::object());
      for (size_t j = i; j < std::min(fresh.size(), i + 200); ++j) {
        json rep = by_ns.count(fresh[j].second) ? by_ns[fresh[j].second] : json::object();
        st.bind(1, fresh[j].second).bind(2, now_seconds()).bind(3, "hub:" + remote).bind(4, rep.dump()).run();
        st.reset();
        record_test_results(s, rep);
      }
    }
  }
  out["result"] = "fast-forward to #" + short_hash(theirs);
  return out;
}

}  // namespace hs
