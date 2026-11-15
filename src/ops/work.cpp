#include "ops/work.h"

#include <cstdlib>
#include <stdexcept>

#include "ops/diff.h"
#include "ops/edit.h"
#include "ops/locks.h"
#include "ops/query.h"
#include "ops/sync.h"
#include "ops/tests.h"

namespace hs {

using json = nlohmann::json;

static bool offline() {
  const char* off = std::getenv("HS_OFFLINE");
  return off && *off;
}

bool coordinated_by_hub(Repo& repo) {
  if (repo.coordination_override() || offline()) return false;
  return lock_transport_override() || remote_url(repo, "origin");
}

std::shared_ptr<Coordination> coordination(Repo& repo) {
  if (auto* c = repo.coordination_override()) return std::shared_ptr<Coordination>(c, [](Coordination*) {});
  if (coordinated_by_hub(repo)) {
    if (auto* t = lock_transport_override()) return hub_coordination(*t);
    auto owned = open_remote(repo, "origin");
    HubTransport& t = *owned;
    return hub_coordination(t, std::move(owned));
  }
  return sql_coordination(repo.store(), repo.config().lock_minutes);
}

// Per agent, in this store: which work item it is on.
static std::string work_ref(Repo& repo) { return "workitem/" + repo.agent(); }

std::string current_work_id(Repo& repo) {
  std::string id = repo.store().ref(work_ref(repo)).value_or("");
  if (!id.empty()) return id;
  WorkItem w;
  w.id = "adhoc-" + repo.agent();
  w.kind = "chore";
  w.title = "ad-hoc edits by " + repo.agent();
  w.owner = repo.agent();
  try {
    coordination(repo)->put_work(w);
  } catch (std::exception&) {
    // Hub unreachable: the lock itself will say so.
  }
  repo.store().set_ref(work_ref(repo), w.id);
  return w.id;
}

std::optional<WorkItem> current_work(Repo& repo) {
  std::string id = repo.store().ref(work_ref(repo)).value_or("");
  if (id.empty()) return std::nullopt;
  return coordination(repo)->work(id);
}

WorkItem start_work(Repo& repo, WorkItem w) {
  if (w.owner.empty()) w.owner = repo.agent();
  if (w.status.empty()) w.status = "open";
  auto c = coordination(repo);
  auto cur = c->work(w.id);
  if (!cur && w.title.empty()) throw std::runtime_error("new work item " + w.id + " needs a title (what it is for)");
  if (cur && cur->status != "open") w.status = "open";  // reopened
  WorkItem out = c->put_work(w);
  repo.store().set_ref(work_ref(repo), out.id);
  return out;
}

WorkItem finish_work(Repo& repo, const std::string& id_in, const std::string& status) {
  if (status != "done" && status != "abandoned") throw std::runtime_error("a work item finishes as done or abandoned");
  std::string id = id_in.empty() ? repo.store().ref(work_ref(repo)).value_or("") : id_in;
  if (id.empty()) throw std::runtime_error("no current work item (hs work start <id> ...)");
  auto c = coordination(repo);
  if (!c->work(id)) throw std::runtime_error("no work item " + id);
  WorkItem w;
  w.id = id;
  w.kind.clear();
  w.status = status;
  WorkItem out = c->put_work(w);
  if (repo.store().ref(work_ref(repo)).value_or("") == id) repo.store().delete_ref(work_ref(repo));
  return out;
}

Request make_request(Repo& repo, const std::string& name, const std::string& kind, const std::string& text,
                     const std::string& test_file, const std::string& test_code) {
  IndexRow row = resolve_name(repo, name);
  if (!test_code.empty() && test_file.empty())
    throw std::runtime_error("test code in a request needs the test file it belongs in (e.g. tests/test_ops.py)");
  Request r;
  r.kind = kind;
  r.target = row.lineage;
  r.name = row.qname;
  r.requester = repo.agent();
  r.requester_work = current_work_id(repo);
  r.text = text;
  r.test_file = test_file;
  r.test_code = test_code;
  return coordination(repo)->add_request(r);
}

static std::string qname_of(Repo& repo, const Request& r) {
  auto rows = rows_by_lineage(repo, r.target);
  return rows.empty() ? r.name : rows.front().qname;
}

Request accept_request(Repo& repo, long long id) {
  auto c = coordination(repo);
  auto r = c->request(id);
  if (!r) throw std::runtime_error("no request R-" + std::to_string(id));
  if (r->holder != repo.agent()) throw std::runtime_error("request R-" + std::to_string(id) + " is for " + r->holder);
  if (!r->is_open()) throw std::runtime_error("request R-" + std::to_string(id) + " is already " + r->status);
  if (!r->test_code.empty() && r->status == "open") {
    // The requester's tests become tests of the definition, so the verify
    // gate holds the change to both agents' expectations.
    std::string target = qname_of(repo, *r);
    Namespace ns = repo.work_ns();
    std::string why = "tests from request R-" + std::to_string(id) + " by " + r->requester;
    EditOutcome o = add_def(repo, ns, r->test_file, "", "", r->test_code, why);
    if (!o.ok) throw std::runtime_error("could not add the request's tests: " + o.error);
    commit_edits(repo, ns, {&o}, why);
    link_tests(repo, target, o.changed);
  }
  return c->update_request(id, "accept", repo.agent(), "", "");
}

Request decline_request(Repo& repo, long long id, const std::string& reason) {
  if (trim(reason).empty()) throw std::runtime_error("say why you decline (the requester reads it)");
  return coordination(repo)->update_request(id, "decline", repo.agent(), reason, "");
}

Request cancel_request(Repo& repo, long long id) {
  return coordination(repo)->update_request(id, "cancel", repo.agent(), "", "");
}

std::vector<Request> waiting_on(Repo& repo, const std::set<std::string>& lineages) {
  std::vector<Request> out;
  if (lineages.empty()) return out;
  RequestFilter f;
  f.agent = repo.agent();
  f.role = "holder";
  for (auto& r : coordination(repo)->requests(f))
    if (r.kind == "change" && r.is_open() && lineages.count(r.target)) out.push_back(r);
  return out;
}

std::string describe_request(const Request& r) {
  std::string s = "R-" + std::to_string(r.id) + " [" + r.status + "] ";
  if (r.kind == "lock") s += r.requester + (r.requester_work.empty() ? "" : " (" + r.requester_work + ")") +
                             " is queued for the lock on " + r.name;
  else s += r.requester + (r.requester_work.empty() ? "" : " (" + r.requester_work + ")") + " asks " + r.holder +
            " to change " + r.name + ": " + r.text;
  if (!r.test_code.empty()) s += " (with tests for " + r.test_file + ")";
  if (!r.note.empty()) s += " -- " + r.note;
  if (!r.commit.empty()) s += " [commit #" + short_hash(r.commit) + "]";
  return s;
}

std::string describe_event(const Event& e) {
  Request r = Request::from_json(e.data.value("request", json::object()));
  std::string id = "R-" + std::to_string(r.id);
  if (e.kind == "request_new")
    return id + ": " + r.requester + (r.requester_work.empty() ? "" : " (" + r.requester_work + ")") +
           (r.kind == "lock" ? " is waiting for your lock on " + r.name
                             : " needs a change in " + r.name + ", which you hold: " + r.text +
                                   ". Accept it, make the change, and commit with resolves [" + std::to_string(r.id) +
                                   "], or decline it");
  if (e.kind == "request_updated") return id + ": " + r.requester + " updated the request on " + r.name + ": " + r.text;
  if (e.kind == "request_accepted") return id + ": " + r.holder + " accepted your request on " + r.name;
  if (e.kind == "request_declined")
    return id + ": " + r.holder + " declined your request on " + r.name + " (" + r.note + ")" +
           (e.data.value("queued_for_lock", json()).is_null() ? "" : "; you are queued for the lock");
  if (e.kind == "request_cancelled") return id + ": " + r.requester + " cancelled the request on " + r.name;
  if (e.kind == "request_done")
    return id + ": done. " + r.holder + " made your change to " + r.name + " in commit #" +
           short_hash(e.data.value("commit", r.commit)) + " (pull to get it)";
  if (e.kind == "lock_granted") return id + ": you now hold the lock on " + r.name + " (" + e.data.value("note", "") + ")";
  return e.kind + ": " + e.data.dump();
}

WorkCommit commit_work(Repo& repo, const std::string& message, const std::vector<long long>& resolves, bool keep_locks) {
  WorkCommit out;
  Store& s = repo.store();
  auto d = diff_namespaces(s, load_namespace(s, repo.head_ns_hash()), repo.work_ns());
  std::set<std::string> touched;
  for (auto& c : d.defs) touched.insert(c.lineage);
  std::set<long long> resolve_ids(resolves.begin(), resolves.end());
  // Committing is local and must work offline: if the hub can't be asked,
  // commit anyway and say so (resolving requests does need it).
  std::shared_ptr<Coordination> coord;
  std::vector<Request> waiting;
  try {
    coord = coordination(repo);
    waiting = waiting_on(repo, touched);
  } catch (std::exception& e) {
    if (!resolve_ids.empty()) throw;
    out.warning = std::string("could not check for requests from other agents (") + e.what() + ")";
    coord.reset();
  }
  std::vector<Request> to_resolve;
  for (long long id : resolve_ids) {
    auto r = coord->request(id);
    if (!r) throw std::runtime_error("no request R-" + std::to_string(id));
    if (r->holder != repo.agent())
      throw std::runtime_error("request R-" + std::to_string(id) + " is for " + r->holder + ", not you");
    if (!r->is_open()) throw std::runtime_error("request R-" + std::to_string(id) + " is already " + r->status);
    if (!touched.count(r->target))
      throw std::runtime_error("this commit doesn't change " + r->name + ", so it can't resolve R-" + std::to_string(id));
    to_resolve.push_back(*r);
  }
  for (auto& r : waiting)
    if (!resolve_ids.count(r.id)) out.left_open.push_back(r);
  if (!out.left_open.empty() && repo.config().requests_on_commit != "warn") {
    std::string msg = "other agents are waiting on changes to what you are committing:";
    for (auto& r : out.left_open) msg += "\n  " + describe_request(r);
    msg += "\nMake the change (accept first to add their tests), then commit with resolves [ids] "
           "(hs commit --resolves 3,4), or decline with a reason (hs request decline <id> --reason \"...\").";
    throw std::runtime_error(msg);
  }
  // Keep the locks until the requests are resolved, so releasing them doesn't
  // hand a lock to a requester whose change was just made.
  out.result = commit(repo, message, true);
  for (auto& r : to_resolve) out.resolved.push_back(coord->update_request(r.id, "resolve", repo.agent(), "", out.result.commit));
  if (coord && !coordinated_by_hub(repo)) out.landed = coord->land({out.result.commit});  // no hub: committed is shared
  if (!keep_locks)
    for (auto& l : list_locks(repo))
      if (l.holder == repo.agent() && touched.count(l.target)) out.result.locks_released += release_lock(repo, l.target);
  return out;
}

static std::string inbox_ref(Repo& repo) {
  std::string where = repo.coordination_override() ? "hub" : coordinated_by_hub(repo) ? "origin" : "local";
  return "inbox/" + where + "/" + repo.agent();
}

std::vector<Event> inbox(Repo& repo, bool advance, int wait_seconds, int limit) {
  long long after = 0;
  try {
    after = std::stoll(repo.store().ref(inbox_ref(repo)).value_or("0"));
  } catch (...) {
  }
  auto c = coordination(repo);
  auto events = wait_seconds > 0 ? c->events_wait(repo.agent(), after, limit, wait_seconds)
                                 : c->events(repo.agent(), after, limit);
  if (advance && !events.empty()) repo.store().set_ref(inbox_ref(repo), std::to_string(events.back().seq));
  return events;
}

json work_status(Repo& repo) {
  auto c = coordination(repo);
  json out = {{"agent", repo.agent()}};
  std::string id = repo.store().ref(work_ref(repo)).value_or("");
  auto w = id.empty() ? std::nullopt : c->work(id);
  out["work"] = w ? w->to_json() : json(nullptr);
  json locks = json::array();
  for (auto& l : c->locks())
    if (l.holder == repo.agent())
      locks.push_back({{"name", l.name}, {"work", l.work}, {"until", format_time(l.expires)}});
  out["locks"] = locks;
  RequestFilter f;
  f.agent = repo.agent();
  json for_me = json::array(), mine = json::array();
  for (auto& r : c->requests(f)) {
    json j = r.to_json();
    j["summary"] = describe_request(r);
    (r.holder == repo.agent() && r.requester != repo.agent() ? for_me : mine).push_back(j);
  }
  out["requests_for_you"] = for_me;
  out["your_requests"] = mine;
  out["inbox"] = inbox(repo, false).size();
  return out;
}

}  // namespace hs
