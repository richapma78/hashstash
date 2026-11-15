#pragma once
// Who may change what on a hub.
//
// Trust starts with the people who run the hub. Accounts are made on the hub
// machine (`hshub user add`), so whoever controls that machine is the root.
// A person signs in to the hub's pages and creates an orchestrator key for a
// project, valid for a number of days. It is shown once and handed to the
// orchestrator (HS_ORCH_KEY). The orchestrator issues worker keys, one per work
// item, optionally narrowed to some files and short-lived (`hs key issue`), and
// gives each to the agent doing that work (HS_WORK_KEY).
//
// A worker key *is* the identity: whoever presents it acts as its work item.
// So when an agent is replaced or starts with a fresh context, the
// orchestrator gives the new one a key and it carries on with the same locks,
// requests and inbox. Agent names are only labels.
//
// Once a project has keys (creating the first orchestrator key switches it on;
// a person can switch it off), changing anything on the hub needs a valid
// key: locks, edits, requests, pushes. Reading stays open to anyone with the
// hub token. Keys are stored as hashes, checked in constant time, expire, and
// can be revoked; revoking an orchestrator key stops every key it issued.
//
// Keys protect a project on a hub. A project without a hub has no referee:
// every agent can write its .hs folder directly.
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

#include "hub/backend.h"

namespace hs {

struct AccessKey {
  std::string id;           // public part of the key
  std::string secret_hash;  // hash of the secret part; the secret itself is never stored
  std::string role;         // orchestrator | worker
  std::string project, work, scope, label;
  std::string issued_by;    // "user:<name>" or "key:<id>"
  long long created = 0, expires = 0, revoked = 0;
  nlohmann::json to_json(bool with_hash = false) const;
  static AccessKey from_json(const nlohmann::json& j);
  bool active(long long now) const { return !revoked && expires > now; }
};

// Who is asking. role "" = no key presented.
struct Principal {
  std::string role, key_id, project, work, scope, label;
  long long expires = 0;
  bool is_worker() const { return role == "worker"; }
  bool is_orchestrator() const { return role == "orchestrator"; }
  nlohmann::json to_json() const;
};

class Access {
 public:
  explicit Access(HubBackend& b) : b_(b) {}

  // People.
  bool has_users();
  void add_user(const std::string& name, const std::string& password);
  // Returns a session token for a cookie; throws on a wrong name or password.
  std::string sign_in(const std::string& name, const std::string& password);
  std::optional<std::string> session_user(const std::string& token);  // nullopt if not signed in
  void sign_out(const std::string& token);

  struct Issued {
    AccessKey key;
    std::string text;  // the full key, shown once: hsk_<id>_<secret>
  };
  // By a signed-in person. Switches the project to requiring keys.
  Issued issue_orchestrator(const std::string& project, const std::string& user, int days, const std::string& label);
  // By an orchestrator. Never outlives the orchestrator key.
  Issued issue_worker(const Principal& by, const std::string& work, const std::string& scope, int hours,
                      const std::string& label);
  // Checks a presented key for project. No key: role "". A key that is wrong,
  // expired, revoked, for another project, or issued by an orchestrator key
  // that is no longer valid: throws.
  Principal authenticate(const std::string& project, const std::string& key_text);
  std::vector<AccessKey> keys(const std::string& project);
  // A person may revoke any key of the project; an orchestrator, worker keys.
  void revoke(const std::string& project, const std::string& id, const std::string& by_user, const Principal* by_key);

  bool required(const std::string& project);
  void set_required(const std::string& project, bool on);

  // Is a definition ("file:qualified.name") or file inside a key's scope?
  // scope: comma-separated globs over file paths or qualified names; "" = all.
  static bool in_scope(const std::string& scope, const std::string& name);

 private:
  Issued make(AccessKey k);
  HubBackend& b_;
};

}  // namespace hs
