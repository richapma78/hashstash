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
//   lock         {target, name, agent, minutes} / unlock {target, agent, force} / locks
//   put_reviews  {reviews: [...]}    -> {stored: n}               reviews made on a client
//   reviews      {after: seq}        -> {reviews: [...], last}    reviews the client hasn't seen
//   verification {ns}                -> {status, report}          test results of a namespace
//                {ns: [...]}         -> {reports: [{ns, status, report}]}
#include <nlohmann/json.hpp>

#include "hub/backend.h"

namespace hs {

class Repo;

// Backends that keep a full local project (SQLite) expose it, so the hub can
// index, query (MCP), verify and render with the client's own code.
struct LocalRepoAccess {
  virtual ~LocalRepoAccess() = default;
  virtual Repo* local_repo() = 0;
};

struct HubOptions {
  bool verify_on_push = false;  // re-run verification on the hub before accepting
};

class HubService {
 public:
  HubService(HubBackend& backend, HubOptions opt = {}) : backend_(backend), opt_(opt) {}
  nlohmann::json handle(const std::string& project, const std::string& op, const nlohmann::json& req);
  // Runs queued background jobs (publish) until none are left. Returns how many ran.
  int run_jobs();
  HubBackend& backend() { return backend_; }

 private:
  nlohmann::json push(ProjectStore& p, const nlohmann::json& req);
  HubBackend& backend_;
  HubOptions opt_;
};

}  // namespace hs
