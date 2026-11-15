#include "hub/service.h"

#include <deque>
#include <set>
#include <stdexcept>
#include <unordered_set>

#include <httplib.h>

#include "core/model.h"
#include "core/util.h"
#include "hub/workspace.h"
#include "ops/publish.h"
#include "ops/repo.h"
#include "ops/tests.h"
#include "ops/verify.h"

namespace hs {

using json = nlohmann::json;

namespace {

Object must(ProjectStore& p, const Hash& h) {
  auto o = p.get(h);
  if (!o) throw std::runtime_error("hub is missing object #" + short_hash(h));
  return *o;
}

// Every object reachable from root that the hub lacks. Stops descending at
// commits in stop (history the hub already holds completely).
std::vector<Hash> missing_closure(ProjectStore& p, const Hash& root, const std::unordered_set<Hash>& stop) {
  std::vector<Hash> missing;
  std::unordered_set<Hash> seen;
  std::deque<Hash> q{root};
  while (!q.empty()) {
    Hash h = q.front();
    q.pop_front();
    if (!seen.insert(h).second || stop.count(h)) continue;
    auto o = p.get(h);
    if (!o) {
      missing.push_back(h);
      continue;
    }
    for (auto& r : object_refs(*o)) q.push_back(r);
  }
  return missing;
}

std::unordered_set<Hash> ancestors(ProjectStore& p, const Hash& head) {
  std::unordered_set<Hash> out;
  std::deque<Hash> q;
  if (!head.empty()) q.push_back(head);
  while (!q.empty()) {
    Hash h = q.front();
    q.pop_front();
    if (!out.insert(h).second) continue;
    auto o = p.get(h);
    if (!o || o->kind != "commit") continue;
    json c = json::parse(o->body);
    for (auto& parent : c.at("parents")) q.push_back(parent);
  }
  return out;
}

// lineage -> definition hash for a namespace (same naming rules as flatten()).
std::map<std::string, Hash> lineage_hashes(ProjectStore& p, const Hash& ns_hash) {
  std::map<std::string, Hash> out;
  if (ns_hash.empty()) return out;
  json ns = json::parse(must(p, ns_hash).body);
  auto lineage = ns.at("lineage");
  std::function<void(const json&, const std::string&, const std::string&)> walk =
      [&](const json& parts, const std::string& file, const std::string& prefix) {
        std::map<std::string, int> seen;
        for (auto& part : parts) {
          if (part.at(0) != "d") continue;
          Hash h = part.at(1);
          json d = json::parse(must(p, h).body);
          std::string name = member_name(d.at("name"), ++seen[d.at("name").get<std::string>()]);
          std::string path = prefix.empty() ? name : prefix + "." + name;
          std::string q = file + ":" + path;
          if (lineage.contains(q)) out[lineage[q].get<std::string>()] = h;
          walk(d.at("parts"), file, path);
        }
      };
  for (auto& [file, e] : ns.at("files").items()) {
    if (e.at(0) != "file") continue;
    walk(json::parse(must(p, e.at(1).get<std::string>()).body).at("parts"), file, "");
  }
  return out;
}

json lock_json(const LockRec& l) { return l.to_json(); }

std::string commit_ns(ProjectStore& p, const Hash& c) {
  if (c.empty()) return "";
  return json::parse(must(p, c).body).at("ns");
}

}  // namespace

HubService::HubService(HubBackend& backend, HubOptions opt) : backend_(backend), opt_(std::move(opt)) {
  if (opt_.cache_dir.empty()) opt_.cache_dir = to_generic(fs::temp_directory_path() / "hshub-cache");
}
HubService::~HubService() = default;

Workspace* HubService::workspace(const std::string& project) {
  auto it = workspaces_.find(project);
  if (it == workspaces_.end()) {
    auto p = backend_.project(project, false);
    if (!p) return nullptr;
    auto ws = std::make_unique<Workspace>(p, from_utf8(opt_.cache_dir), opt_.force_cache);
    it = workspaces_.emplace(project, std::move(ws)).first;
  }
  it->second->refresh();
  return it->second.get();
}

json HubService::push(ProjectStore& p, const json& req) {
  std::string branch = req.value("branch", "main");
  Hash old_head = req.value("old", ""), new_head = req.at("new");
  std::string agent = req.value("agent", "someone");

  auto o = p.get(new_head);
  if (!o || o->kind != "commit") throw std::runtime_error("push: commit #" + short_hash(new_head) + " not uploaded");
  auto known = ancestors(p, old_head);
  auto missing = missing_closure(p, new_head, known);
  if (!missing.empty())
    throw std::runtime_error("push: " + std::to_string(missing.size()) + " objects missing, e.g. #" +
                             short_hash(missing[0]));
  if (!old_head.empty() && !ancestors(p, new_head).count(old_head))
    throw std::runtime_error("push: not a fast-forward of " + branch + "; pull and merge first");

  // Locks: the pushing agent's locks replace its previous set on the hub; a
  // change to a definition someone else holds is refused.
  Hash old_ns = commit_ns(p, old_head), new_ns = commit_ns(p, new_head);
  auto before = lineage_hashes(p, old_ns), after = lineage_hashes(p, new_ns);
  std::set<std::string> changed;
  for (auto& [id, h] : after)
    if (!before.count(id) || before[id] != h) changed.insert(id);
  for (auto& [id, h] : before)
    if (!after.count(id)) changed.insert(id);
  for (auto& l : p.coord().locks())
    if (changed.count(l.target) && l.holder != agent)
      throw std::runtime_error("push: " + l.name + " is locked on the hub by " + l.holder +
                               (l.work.empty() ? "" : " for " + l.work));

  // Verification gate.
  std::string status = req.value("/verification/status"_json_pointer, std::string());
  if (status != "passed") throw std::runtime_error("push: commit is not verified (run hs verify, then push)");
  std::string report = req.contains("verification") && req["verification"].contains("report")
                           ? req["verification"]["report"].dump()
                           : "{}";
  if (opt_.verify_on_push) {
    Workspace* ws = workspace(p.name());
    ws->fetch(new_head);
    Repo& r = ws->repo();
    Hash keep = r.work_ns_hash();
    r.set_work_ns_hash(new_ns, "verify", "hub verification of #" + short_hash(new_head));
    json v = verify(r, false);
    if (v["status"] != "passed") {
      r.set_work_ns_hash(keep, "rollback", "hub verification failed");
      throw std::runtime_error("push: hub verification failed: " + v["failures"].dump());
    }
    report = v.dump();
    r.set_work_ns_hash(keep, "verify", "hub verification done");
  }
  // The report carries the test results; workspaces read them from here.
  p.set_verification(new_ns, "passed", report);

  if (!p.cas_branch(branch, old_head, new_head))
    throw std::runtime_error("push: " + branch + " moved on the hub; pull and retry");
  p.log_push(branch, old_head, new_head, agent);

  // Requests resolved by the commits this push brought are done now.
  std::vector<std::string> landed;
  for (auto& c : ancestors(p, new_head))
    if (!known.count(c)) landed.push_back(c);
  int done = p.coord().land(landed);

  // This agent's locks become the set it sent (released ones go to whoever queued).
  std::vector<LockRec> keep;
  for (auto& l : req.value("locks", json::array()))
    keep.push_back({l.at("target"), l.value("name", ""), agent, l.value("work", ""), 0, l.value("expires", 0LL)});
  p.coord().sync_locks(agent, keep);

  // Keep the hub's working state, index and graph at the branch head.
  Workspace* ws = workspace(p.name());
  if (ws && ws->repo().branch() == branch && publish_on_push(ws->repo()))
    backend_.enqueue({0, p.name(), "publish", branch});
  return {{"head", new_head}, {"branch", branch}, {"requests_done", done}};
}

json HubService::handle(const std::string& project, const std::string& op, const json& req) {
  if (op == "create") {
    backend_.project(project, true);
    return {{"created", project}};
  }
  auto p = backend_.project(project, false);
  if (!p) throw std::runtime_error("no project '" + project + "' on this hub");

  if (op == "refs") {
    json locks = json::array();
    for (auto& l : p->coord().locks()) locks.push_back(lock_json(l));
    return {{"branches", p->branches()}, {"locks", locks}};
  }
  if (op == "missing") {
    json out = json::array();
    for (auto& h : req.at("hashes"))
      if (!p->has(h)) out.push_back(h);
    return {{"missing", out}};
  }
  if (op == "put_objects") {
    int n = 0;
    p->begin();
    try {
      for (auto& o : req.at("objects")) {
        Object obj{o.at("k"), base64_decode(o.at("b").get<std::string>())};
        Hash h = o.at("h");
        if (hash_bytes(obj.kind, obj.body) != h) throw std::runtime_error("object #" + short_hash(h) + " doesn't match its hash");
        p->put(h, obj);
        ++n;
      }
      p->commit();
    } catch (...) {
      p->rollback();
      throw;
    }
    return {{"stored", n}};
  }
  if (op == "get_objects") {
    json out = json::array();
    for (auto& h : req.at("hashes")) {
      auto o = p->get(h);
      if (o) out.push_back({{"h", h}, {"k", o->kind}, {"b", base64_encode(o->body)}});
    }
    return {{"objects", out}};
  }
  if (op == "reachable") {
    std::unordered_set<Hash> have;
    for (auto& h : req.value("have", json::array())) have.insert(h);
    std::vector<Hash> out;
    std::unordered_set<Hash> seen;
    std::deque<Hash> q{req.at("from").get<std::string>()};
    while (!q.empty()) {
      Hash h = q.front();
      q.pop_front();
      if (!seen.insert(h).second || have.count(h)) continue;
      auto o = p->get(h);
      if (!o) continue;
      out.push_back(h);
      for (auto& r : object_refs(*o)) q.push_back(r);
    }
    return {{"hashes", out}};
  }
  if (op == "push") return push(*p, req);
  if (op == "put_reviews") {
    int n = 0;
    for (auto& r : req.at("reviews")) {
      p->put_review(r);
      ++n;
    }
    return {{"stored", n}};
  }
  if (op == "reviews") return p->reviews_after(req.value("after", 0LL));
  if (op == "verification") {
    auto one = [&](const Hash& ns) {
      std::string rep = p->verification_report(ns);
      json out = {{"ns", ns}, {"status", p->verification(ns)}};
      try {
        out["report"] = rep.empty() ? json::object() : json::parse(rep);
      } catch (...) {
        out["report"] = json::object();
      }
      return out;
    };
    if (req.at("ns").is_string()) return one(req["ns"]);
    json all = json::array();
    for (auto& ns : req["ns"]) all.push_back(one(ns));
    return {{"reports", all}};
  }
  if (op == "lock") {
    long long minutes = req.value("minutes", 15);
    long long now = now_seconds();
    LockRec l{req.at("target"), req.value("name", ""), req.at("agent"), req.value("work", ""), now, now + 60 * minutes};
    if (auto b = p->coord().acquire(l)) return {{"ok", false}, {"held_by", lock_json(*b)}};
    return {{"ok", true}, {"lock", lock_json(l)}};
  }
  if (op == "unlock")
    return {{"released", p->coord().release(req.at("target"), req.value("agent", ""), req.value("force", false))}};
  if (op == "locks") {
    json locks = json::array();
    for (auto& l : p->coord().locks()) locks.push_back(lock_json(l));
    return {{"locks", locks}};
  }
  if (op == "coord") return coord_dispatch(p->coord(), req);
  throw std::runtime_error("unknown operation " + op);
}

int HubService::run_jobs() {
  int n = 0;
  while (auto j = backend_.next_job()) {
    std::string result;
    try {
      Workspace* ws = workspace(j->project);
      if (!ws) throw std::runtime_error("no project " + j->project);
      if (j->kind == "publish") result = publish(ws->repo(), j->arg).dump();
      else result = "unknown job kind";
    } catch (std::exception& e) {
      result = std::string("error: ") + e.what();
    }
    backend_.finish_job(j->id, result);
    ++n;
  }
  return n;
}

// Webhooks: each subscription gets its agent's new events, in order, as
// {project, agent, events}. A failed post is retried on the next round.
int HubService::deliver_webhooks() {
  int posts = 0;
  for (auto& name : backend_.projects()) {
    auto p = backend_.project(name, false);
    if (!p) continue;
    Coordination& c = p->coord();
    for (auto& sub : c.subscriptions()) {
      auto events = c.events(sub.agent, sub.after, 50);
      if (events.empty()) continue;
      json arr = json::array();
      for (auto& e : events) arr.push_back(e.to_json());
      size_t scheme = sub.url.find("://");
      size_t slash = sub.url.find('/', scheme + 3);
      std::string base = sub.url.substr(0, slash);
      std::string path = slash == std::string::npos ? "/" : sub.url.substr(slash);
      try {
        httplib::Client cli(base);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(10);
        auto res = cli.Post(path, json{{"project", name}, {"agent", sub.agent}, {"events", arr}}.dump(), "application/json");
        ++posts;
        if (res && res->status >= 200 && res->status < 300) c.set_subscription_cursor(sub.id, events.back().seq);
      } catch (...) {
      }
    }
  }
  return posts;
}

}  // namespace hs
