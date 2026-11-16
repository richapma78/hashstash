#pragma once
// Coordination between agents: leased locks tagged with the work item they are
// for, work items, requests queued on locked definitions, and the event feed
// that tells an agent when something it waits for has happened.
//
// One interface, several homes:
//   sql_coordination   a project store: a local project shared by several
//                      agents, or a hub project kept in SQLite
//   hub_coordination   a client whose project has a hub: every call goes to the
//                      hub, which is the single authority
//   (scaled backends)  the SQL in coord.cpp is portable (Postgres runs it with
//                      the same statements), or a backend implements this
//                      interface on its own store
// Every state change is one transaction, so many hub servers can share one
// database: nothing here lives in process memory.
//
// Requests: an agent that needs a definition someone else has locked logs a
// request on it instead of waiting.
//   change  "please make this change while you have it": the holder must deal
//           with it before committing that definition (accept and resolve it
//           in the commit, or decline it). It is done when the resolving commit
//           reaches the shared branch (pushed to the hub, or committed when
//           there is no hub), and the requester is told.
//   lock    "give me the lock next": granted when the lock is released or its
//           lease runs out, highest priority first, then oldest.
// A lock released with change requests still open hands the lock to the first
// requester, so nothing stalls on an agent that went away.
#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/store.h"

namespace hs {

class HubTransport;

struct LockRec {
  std::string target, name, holder, work;
  long long acquired = 0, expires = 0;
  nlohmann::json to_json() const;
  static LockRec from_json(const nlohmann::json& j);
};

struct WorkItem {
  std::string id, kind, title, owner, link, status;  // empty kind/status: unchanged (new: feature, open)
  int priority = -1;  // -1: default for the kind (bug 2, feature 1, chore 0)
  long long created = 0, closed = 0;
  nlohmann::json to_json() const;
  static WorkItem from_json(const nlohmann::json& j);
};

struct Request {
  long long id = 0;
  std::string kind = "change";  // change | lock
  std::string target, name, requester, requester_work, holder, holder_work, text, test_file, test_code;
  std::string status = "open";  // open accepted resolved done declined cancelled granted returned
  std::string commit, note;
  // Filled in when the request is made, so any agent can act on it later:
  // {version: def hash the requester saw, signature, requester_work_title,
  //  requester_holds: [names the requester has checked out]}
  nlohmann::json context = nlohmann::json::object();
  int priority = -1;  // -1: the requester's work item priority
  long long created = 0, updated = 0;
  bool is_open() const { return status == "open" || status == "accepted"; }
  nlohmann::json to_json() const;
  static Request from_json(const nlohmann::json& j);
};

struct Event {
  long long seq = 0;
  std::string agent, work, kind;
  nlohmann::json data;
  long long time = 0;
  nlohmann::json to_json() const;
  static Event from_json(const nlohmann::json& j);
};

struct Subscription {
  long long id = 0;
  std::string agent, url;
  long long after = 0;
  nlohmann::json to_json() const;
  static Subscription from_json(const nlohmann::json& j);
};

struct RequestFilter {
  std::string agent;          // "" = anyone
  std::string role;           // holder | requester | "" (either)
  std::string target;         // "" = any definition
  bool open_only = true;      // open, accepted or resolved (not yet landed)
  nlohmann::json to_json() const;
  static RequestFilter from_json(const nlohmann::json& j);
};

class Coordination {
 public:
  virtual ~Coordination() = default;

  // Locks (unexpired only). acquire returns the blocking lock, or nullopt when
  // taken. release hands the lock to the first queued request, if any.
  virtual std::vector<LockRec> locks() = 0;
  virtual std::optional<LockRec> acquire(const LockRec& l) = 0;
  virtual bool release(const std::string& target, const std::string& holder, bool force) = 0;
  // The holder's locks become exactly keep (used when a push carries locks).
  virtual void sync_locks(const std::string& holder, const std::vector<LockRec>& keep) = 0;

  virtual WorkItem put_work(const WorkItem& w) = 0;  // create, or update the given fields
  virtual std::optional<WorkItem> work(const std::string& id) = 0;
  virtual std::vector<WorkItem> works(bool include_closed) = 0;

  // Fills in holder, priority and status, and tells the holder.
  virtual Request add_request(const Request& r) = 0;
  virtual std::optional<Request> request(long long id) = 0;
  virtual std::vector<Request> requests(const RequestFilter& f) = 0;
  // action: accept | decline | resolve (by the holder), cancel (by the requester).
  virtual Request update_request(long long id, const std::string& action, const std::string& actor,
                                 const std::string& note, const std::string& commit) = 0;
  // These commits reached the shared branch: resolved requests they carry are
  // done. verified is the commit whose verify passed (the hub's new head, or
  // the local head): the request records it, since the resolving commit may
  // not hold the whole change (it can be finished in later commits).
  virtual int land(const std::vector<std::string>& commits, const std::string& verified = "") = 0;

  // Events for agent (sent to it, or to a work item it owns) after seq.
  virtual std::vector<Event> events(const std::string& agent, long long after, int limit) = 0;
  virtual long long last_event() = 0;
  // Like events, but waits up to wait_seconds for one to arrive (long poll).
  virtual std::vector<Event> events_wait(const std::string& agent, long long after, int limit, int wait_seconds);

  // Webhook subscriptions: the hub posts an agent's events to url.
  virtual Subscription subscribe(const std::string& agent, const std::string& url) = 0;
  virtual bool unsubscribe(const std::string& agent, const std::string& url) = 0;
  virtual std::vector<Subscription> subscriptions() = 0;
  virtual void set_subscription_cursor(long long id, long long after) = 0;
};

// lease_minutes: how long a lock handed to a queued requester lasts.
std::unique_ptr<Coordination> sql_coordination(Store& s, int lease_minutes = 15);
// Every call goes to the hub. With owned null, t must outlive the result.
std::unique_ptr<Coordination> hub_coordination(HubTransport& t, std::unique_ptr<HubTransport> owned = nullptr);
// Serves a hub "coord" call ({method, ...}) on c.
nlohmann::json coord_dispatch(Coordination& c, const nlohmann::json& req);

int default_priority(const std::string& kind);

}  // namespace hs
