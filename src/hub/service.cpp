#include "hub/service.h"

#include <deque>
#include <set>
#include <stdexcept>
#include <unordered_set>

#include "core/model.h"
#include "core/util.h"
#include "ops/publish.h"
#include "ops/repo.h"
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

json lock_json(const HubLock& l) {
  return {{"target", l.target}, {"name", l.name}, {"holder", l.holder}, {"expires", l.expires}};
}

std::string commit_ns(ProjectStore& p, const Hash& c) {
  if (c.empty()) return "";
  return json::parse(must(p, c).body).at("ns");
}

}  // namespace

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
  for (auto& l : p.locks())
    if (changed.count(l.target) && l.holder != agent)
      throw std::runtime_error("push: " + l.name + " is locked on the hub by " + l.holder);

  // Verification gate.
  std::string status = req.value("/verification/status"_json_pointer, std::string());
  if (status != "passed") throw std::runtime_error("push: commit is not verified (run hs verify, then push)");
  std::string report = req.contains("verification") && req["verification"].contains("report")
                           ? req["verification"]["report"].dump()
                           : "{}";
  auto* local = dynamic_cast<LocalRepoAccess*>(&p);
  if (opt_.verify_on_push && local && local->local_repo()) {
    Repo& r = *local->local_repo();
    Hash keep = r.work_ns_hash();
    r.set_work_ns_hash(new_ns, "verify", "hub verification of #" + short_hash(new_head));
    json v = verify(r, false);
    if (v["status"] != "passed") {
      r.set_work_ns_hash(keep, "rollback", "hub verification failed");
      throw std::runtime_error("push: hub verification failed: " + v["failures"].dump());
    }
    report = v.dump();
  }
  p.set_verification(new_ns, "passed", report);

  if (!p.cas_branch(branch, old_head, new_head))
    throw std::runtime_error("push: " + branch + " moved on the hub; pull and retry");

  // Sync this agent's locks.
  for (auto& l : p.locks())
    if (l.holder == agent) p.release(l.target, agent, false);
  if (req.contains("locks"))
    for (auto& l : req["locks"]) p.acquire({l.at("target"), l.at("name"), agent, l.at("expires")});

  // Keep the hub's own working state, index and graph at the branch head.
  if (local && local->local_repo()) {
    Repo& r = *local->local_repo();
    if (r.branch() == branch) {
      json c = json::parse(must(p, new_head).body);
      r.set_work_ns_hash(new_ns, "push", agent + " pushed #" + short_hash(new_head) + ": " + c.value("message", ""));
      if (publish_on_push(r)) backend_.enqueue({0, p.name(), "publish", branch});
    }
  }
  return {{"head", new_head}, {"branch", branch}};
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
    for (auto& l : p->locks()) locks.push_back(lock_json(l));
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
  if (op == "lock") {
    long long minutes = req.value("minutes", 15);
    HubLock l{req.at("target"), req.value("name", ""), req.at("agent"), now_seconds() + 60 * minutes};
    if (auto b = p->acquire(l)) return {{"ok", false}, {"held_by", lock_json(*b)}};
    return {{"ok", true}, {"lock", lock_json(l)}};
  }
  if (op == "unlock") return {{"released", p->release(req.at("target"), req.value("agent", ""), req.value("force", false))}};
  if (op == "locks") {
    json locks = json::array();
    for (auto& l : p->locks()) locks.push_back(lock_json(l));
    return {{"locks", locks}};
  }
  throw std::runtime_error("unknown operation " + op);
}

int HubService::run_jobs() {
  int n = 0;
  while (auto j = backend_.next_job()) {
    std::string result;
    try {
      auto p = backend_.project(j->project, false);
      auto* local = p ? dynamic_cast<LocalRepoAccess*>(p.get()) : nullptr;
      if (!local || !local->local_repo()) throw std::runtime_error("no local project for job");
      if (j->kind == "publish") result = publish(*local->local_repo(), j->arg).dump();
      else result = "unknown job kind";
    } catch (std::exception& e) {
      result = std::string("error: ") + e.what();
    }
    backend_.finish_job(j->id, result);
    ++n;
  }
  return n;
}

}  // namespace hs
