#pragma once
// The hub's sync protocol, independent of transport and storage. The HTTP
// server (hshub) and in-process tests both call handle().
//
// Operations (request/response are JSON):
//   refs         -> {branches: {name: commit}, locks: [...]}
//   missing      {hashes}            -> {missing: [...]}           which objects the hub lacks
//   put_objects  {objects: [{h,k,b}]} -> {stored: n}              b = base64 body; hashes checked
//   get_objects  {hashes}            -> {objects: [{h,k,b}]}
//   reachable    {from, have}        -> {hashes: [...]}           closure of from, minus commits in have
//   push         {branch, old, new, agent, verification, locks} -> {head}
//   lock         {target, name, agent, work, minutes} / unlock {target, agent, force} / locks
//   whoami                           -> the presented access key: {role, work, scope, ...}
//   issue_key    {work, title, kind, priority, scope, hours, label} -> {key, text}   (orchestrator)
//   keys / revoke_key {id}                                                           (orchestrator)
//   coord        {method, ...}       -> coordination call (ops/coord.h): locks, work
//                                       items, requests, events (wait = long poll,
//                                       answered by the HTTP server), subscriptions
//   put_reviews  {reviews: [...]}    -> {stored: n}               reviews made on a client
//   reviews      {after: seq}        -> {reviews: [...], last}    reviews the client hasn't seen
//   verification {ns}                -> {status, report}          test results of a namespace
//                {ns: [...]}         -> {reports: [{ns, status, report}]}
#include <nlohmann/json.hpp>

#include <map>
#include <memory>

#include "hub/access.h"
#include "hub/backend.h"

namespace hs {

class Workspace;

struct HubOptions {
  bool verify_on_push = false;  // re-run verification on the hub before accepting
  // Where cached workspaces live, for backends that aren't hashstash projects
  // themselves ("" = <system temp>/hshub-cache). force_cache uses a cached
  // workspace even when the backend offers its own project (tests, and
  // checking that a hub works the way a scaled one would).
  std::string cache_dir;
  bool force_cache = false;
};

class HubService {
 public:
  HubService(HubBackend& backend, HubOptions opt = {});
  ~HubService();
  // key: the access key the caller presented ("" = none). On a project that
  // requires keys, operations that change anything need one; a worker key
  // acts as its work item, inside its scope (see hub/access.h).
  nlohmann::json handle(const std::string& project, const std::string& op, const nlohmann::json& req,
                        const std::string& key = "");
  // Runs queued background jobs (publish) until none are left. Returns how many ran.
  int run_jobs();
  // Posts new events to webhook subscribers. Returns how many posts were made.
  int deliver_webhooks();
  HubBackend& backend() { return backend_; }
  Access& access() { return access_; }
  // The project's workspace, brought up to date (nullptr if no such project).
  Workspace* workspace(const std::string& project);

 private:
  nlohmann::json push(ProjectStore& p, const nlohmann::json& req, const Principal& who);
  nlohmann::json handle_op(const std::string& project, const std::string& op, const nlohmann::json& req,
                           const Principal& who);
  HubBackend& backend_;
  Access access_;
  HubOptions opt_;
  std::map<std::string, std::unique_ptr<Workspace>> workspaces_;
};

}  // namespace hs
