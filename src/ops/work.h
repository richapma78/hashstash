#pragma once
// Work items, change requests and the inbox, for the agent a Repo acts as.
// These are the operations behind `hs work`, `hs request`, `hs inbox` and the
// matching MCP tools; the data lives behind ops/coord.h.
//
// The flow when two agents need the same definition:
//   1. Agent B finds mul locked by A (for FEAT-12) and logs a request on it:
//      what it needs changed, optionally with test code that must pass.
//   2. A is told (inbox / MCP channel / webhook). When A commits mul, the
//      commit is refused while B's request is open, so A takes it on:
//      accept (B's tests are added and linked to mul, so verify runs them),
//      make the change, verify, and commit with resolves: [R-id]. Or decline.
//   3. When A's commit reaches the shared branch (pushed to the hub), the
//      request is done and B is told, with the commit. B pulls and goes on.
#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ops/coord.h"
#include "ops/history.h"
#include "ops/repo.h"

namespace hs {

// Where this project's coordination lives: the repo's override (the hub's own
// workspace), the hub when a remote "origin" is set (and HS_OFFLINE isn't),
// otherwise this store.
std::shared_ptr<Coordination> coordination(Repo& repo);
// True when coordination goes to a hub over the network (so commits land on push).
bool coordinated_by_hub(Repo& repo);

// The agent's current work item id. Locks, requests and commits carry it.
// When the agent has none, an ad-hoc chore item is made for it.
std::string current_work_id(Repo& repo);
std::optional<WorkItem> current_work(Repo& repo);
// Creates the item (or updates the fields given) and makes it current.
WorkItem start_work(Repo& repo, WorkItem w);
// Closes a work item: status done or abandoned. Current item by default.
WorkItem finish_work(Repo& repo, const std::string& id, const std::string& status = "done");

// Logs a request on a definition another agent holds. kind: change | lock.
Request make_request(Repo& repo, const std::string& name, const std::string& kind, const std::string& text,
                     const std::string& test_file = "", const std::string& test_code = "");
// Accept: adds the request's test code to test_file and links it to the
// definition (so verify runs the requester's tests too), then marks it accepted.
Request accept_request(Repo& repo, long long id);
Request decline_request(Repo& repo, long long id, const std::string& reason);
Request cancel_request(Repo& repo, long long id);
// Open change requests others logged on these definitions (lineage ids), where
// this agent is the holder.
std::vector<Request> waiting_on(Repo& repo, const std::set<std::string>& lineages);

struct WorkCommit {
  CommitResult result;
  std::vector<Request> resolved;   // marked resolved by this commit
  std::vector<Request> left_open;  // change requests on what was committed, not resolved ([work] requests = "warn")
  int landed = 0;                  // requests done now (no hub: committing is sharing)
  std::string warning;             // e.g. the hub couldn't be asked about requests
};
// commit() with the request gate: a commit that changes a definition with open
// change requests from others must resolve them (resolves = request ids) or is
// refused ([work] requests = "block", the default).
WorkCommit commit_work(Repo& repo, const std::string& message, const std::vector<long long>& resolves = {},
                       bool keep_locks = false);

// Events for this agent since its cursor. advance moves the cursor past them;
// wait_seconds > 0 waits for one (long poll).
std::vector<Event> inbox(Repo& repo, bool advance, int wait_seconds = 0, int limit = 100);
// One line for a person or an agent: "R-3 from agent-b (FEAT-13) on mul: ...".
std::string describe_event(const Event& e);
std::string describe_request(const Request& r);
// Current work item, its locks, requests to and from this agent, inbox count.
nlohmann::json work_status(Repo& repo);

}  // namespace hs
