#pragma once
// Hub pages for people: sign in, and per project create orchestrator keys,
// see and revoke keys, and switch key protection on or off.
//
//   /ui/login, /ui/logout
//   /ui/p/<project>/access          keys of the project and the forms
//
// Accounts are made on the hub machine (`hshub user add`). Until one exists,
// the access page says how to set it up and no orchestrator key can be made.
#include <functional>
#include <mutex>
#include <string>

namespace httplib {
class Server;
struct Request;
}  // namespace httplib

namespace hs {

class Access;

struct AccessSite {
  Access* access = nullptr;
  std::function<bool(const std::string&)> project_exists;
  std::mutex* mu = nullptr;
};

void mount_access_ui(httplib::Server& srv, const AccessSite& site);
// The signed-in person for a request ("" if none).
std::string signed_in_user(Access& access, const httplib::Request& req);
std::string cookie_value(const httplib::Request& req, const std::string& name);
// The sign-in form (shown in place of any /ui page when not signed in).
std::string sign_in_page(const std::string& message);

}  // namespace hs
