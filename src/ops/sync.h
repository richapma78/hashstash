#pragma once
// Client side of hub sync: remotes, push, pull, clone, and shared locks.
//
// push  sends the objects the hub lacks, then asks it to move the branch from
//       the head we last saw to ours (fast-forward only). The head must be
//       verified. Our locks travel with the push.
// pull  fetches the objects we lack and fast-forwards our branch; the hub's
//       locks come back with it, so local edits respect them.
// When a remote "origin" is set, taking or releasing a lock also takes or
// releases it on the hub, so agents on different machines can't collide.
#include <nlohmann/json.hpp>

#include <memory>
#include <string>

#include "ops/repo.h"

namespace hs {

class HubTransport {
 public:
  virtual ~HubTransport() = default;
  // Calls one hub operation for this transport's project. Throws on errors.
  virtual nlohmann::json call(const std::string& op, const nlohmann::json& req) = 0;
};

// url: http://host:port/<project>
std::unique_ptr<HubTransport> http_transport(const std::string& url);

void set_remote(Repo& repo, const std::string& name, const std::string& url);
std::optional<std::string> remote_url(Repo& repo, const std::string& name);
std::unique_ptr<HubTransport> open_remote(Repo& repo, const std::string& name);

nlohmann::json push(Repo& repo, HubTransport& hub, const std::string& remote = "origin",
                    const std::string& branch = "");
nlohmann::json pull(Repo& repo, HubTransport& hub, const std::string& remote = "origin",
                    const std::string& branch = "");

// Test hook: route lock calls for remote "origin" through this transport
// instead of opening an HTTP connection.
void set_lock_transport_override(HubTransport* t);
HubTransport* lock_transport_override();

// Reviews: send the ones made here, fetch the ones made elsewhere. push and
// pull do both; these are for sending a new review right away.
int send_reviews(Repo& repo, HubTransport& hub);
int fetch_reviews(Repo& repo, HubTransport& hub, const std::string& remote);
// Sends unsent reviews to "origin" if one is set (and HS_OFFLINE isn't).
// Returns a note for the user: "" on success or when there is no hub.
std::string share_reviews(Repo& repo);

}  // namespace hs
