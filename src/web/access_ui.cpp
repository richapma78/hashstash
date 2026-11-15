#include "web/access_ui.h"

#include <httplib.h>

#include "core/util.h"
#include "hub/access.h"
#include "web/ui.h"

namespace hs {

std::string cookie_value(const httplib::Request& req, const std::string& name) {
  std::string all = req.get_header_value("Cookie");
  size_t pos = 0;
  while (pos < all.size()) {
    size_t end = all.find(';', pos);
    if (end == std::string::npos) end = all.size();
    std::string part = trim(all.substr(pos, end - pos));
    if (starts_with(part, name + "=")) return part.substr(name.size() + 1);
    pos = end + 1;
  }
  return "";
}

std::string signed_in_user(Access& access, const httplib::Request& req) {
  return access.session_user(cookie_value(req, "hs_session")).value_or("");
}

std::string sign_in_page(const std::string& message) {
  return ui_page("Sign in", "",
                 "<h1>Sign in</h1>" +
                     (message.empty() ? std::string() : "<div class=\"flash bad\">" + html_esc(message) + "</div>") +
                     "<form class=\"review\" method=\"post\" action=\"/ui/login\">"
                     "<p><label>Name <input type=\"text\" name=\"name\" autofocus autocomplete=\"username\"></label></p>"
                     "<p><label>Password <input type=\"password\" name=\"password\" autocomplete=\"current-password\">"
                     "</label></p><button>Sign in</button></form>"
                     "<p class=\"meta\">Accounts are made on the hub machine: <code>hshub user add &lt;name&gt;</code></p>");
}

namespace {

std::string when(long long t) { return t ? format_time(t) : ""; }

std::string key_status(const AccessKey& k, long long now) {
  if (k.revoked) return "<span class=\"badge bad\">revoked</span>";
  if (k.expires <= now) return "<span class=\"badge muted\">expired</span>";
  return "<span class=\"badge good\">active</span>";
}

std::string access_page(Access& a, const std::string& project, const std::string& user, const std::string& flash) {
  std::string P = "/ui/p/" + url_encode(project) + "/access";
  std::string o = "<h1>Agent access</h1>" + flash;
  if (!a.has_users()) {
    return o +
           "<p>Signing in isn't set up on this hub yet, so no keys can be made. On the hub machine run:</p>"
           "<pre class=\"code\">hshub user add &lt;your name&gt; --root &lt;hub folder&gt;</pre>"
           "<p>then sign in here and create an orchestrator key.</p>";
  }
  bool req = a.required(project);
  o += "<p>Signed in as <b>" + html_esc(user) + "</b> &middot; <a href=\"/ui/logout\">sign out</a></p>";
  o += "<div class=\"cards\"><div class=\"card\"><div class=\"meta\">Agents need a key to change code</div><div class=\"n\">" +
       std::string(req ? "Yes" : "No") + "</div><form method=\"post\" action=\"" + P + "/require\">" +
       "<input type=\"hidden\" name=\"on\" value=\"" + (req ? "0" : "1") + "\"><button>" +
       (req ? "Switch off" : "Switch on") + "</button></form></div></div>";
  o += "<h2>New orchestrator key</h2>"
       "<form class=\"review\" method=\"post\" action=\"" + P + "/orchestrator\">"
       "<label>Label <input type=\"text\" name=\"label\" placeholder=\"e.g. ork-1\"></label> "
       "<label>Valid for <input type=\"text\" name=\"days\" value=\"30\" size=\"4\"> days</label> "
       "<button>Create key</button>"
       "<p class=\"meta\">The key is shown once. Give it to the orchestrator agent as <code>HS_ORCH_KEY</code> "
       "(in its environment, not in a chat). It issues the keys its workers use. Creating a key switches "
       "key protection on for this project.</p></form>";
  o += "<h2>Keys</h2><table><tr><th>Key</th><th>Role</th><th>Work item</th><th>Scope</th><th>Label</th>"
       "<th>Issued by</th><th>Created</th><th>Expires</th><th>Status</th><th></th></tr>";
  long long now = now_seconds();
  auto keys = a.keys(project);
  for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
    auto& k = *it;
    o += "<tr><td class=\"h\">" + html_esc(k.id) + "</td><td>" + html_esc(k.role) + "</td><td>" + html_esc(k.work) +
         "</td><td class=\"h\">" + html_esc(k.scope.empty() ? (k.role == "worker" ? "all" : "") : k.scope) + "</td><td>" +
         html_esc(k.label) + "</td><td>" + html_esc(k.issued_by) + "</td><td>" + when(k.created) + "</td><td>" +
         when(k.expires) + "</td><td>" + key_status(k, now) + "</td><td>";
    if (k.active(now))
      o += "<form method=\"post\" action=\"" + P + "/revoke\"><input type=\"hidden\" name=\"id\" value=\"" + html_esc(k.id) +
           "\"><button>Revoke</button></form>";
    o += "</td></tr>";
  }
  if (keys.empty()) o += "<tr><td colspan=\"10\" class=\"meta\">No keys yet.</td></tr>";
  o += "</table><p class=\"meta\">Revoking an orchestrator key also stops every worker key it issued.</p>";
  return ui_page("Access - " + project, project, o);
}

}  // namespace

void mount_access_ui(httplib::Server& srv, const AccessSite& site_in) {
  auto site = std::make_shared<AccessSite>(site_in);
  const std::string P = R"(/ui/p/([A-Za-z0-9._-]+)/access)";
  // Runs fn for a signed-in person with the hub lock held; errors become a page.
  auto run = [site](const httplib::Request& req, httplib::Response& res,
                    const std::function<std::string(const std::string& project, const std::string& user)>& fn) {
    std::unique_lock<std::mutex> g;
    if (site->mu) g = std::unique_lock<std::mutex>(*site->mu);
    res.set_header("Cache-Control", "no-store");
    try {
      std::string project = req.matches[1];
      if (!site->project_exists(project)) throw std::runtime_error("no project " + project);
      std::string user = signed_in_user(*site->access, req);
      if (site->access->has_users() && user.empty()) {
        res.set_content(sign_in_page(""), "text/html; charset=utf-8");
        return;
      }
      std::string body = fn(project, user);
      if (!body.empty()) res.set_content(body, "text/html; charset=utf-8");
    } catch (std::exception& e) {
      res.status = 400;
      res.set_content(ui_page("Error", "", "<h1>Something went wrong</h1><p>" + html_esc(e.what()) + "</p>"),
                      "text/html; charset=utf-8");
    }
  };
  auto need_user = [site](const std::string& user) {
    if (!site->access->has_users() || user.empty())
      throw std::runtime_error("sign in first (accounts are made on the hub machine: hshub user add <name>)");
  };

  srv.Get(P, [=](const httplib::Request& req, httplib::Response& res) {
    run(req, res, [&](const std::string& project, const std::string& user) {
      return access_page(*site->access, project, user, "");
    });
  });
  srv.Post(P + "/orchestrator", [=](const httplib::Request& req, httplib::Response& res) {
    run(req, res, [&](const std::string& project, const std::string& user) {
      need_user(user);
      int days = 30;
      try {
        days = std::stoi(req.get_param_value("days"));
      } catch (...) {
      }
      auto issued = site->access->issue_orchestrator(project, user, days, trim(req.get_param_value("label")));
      std::string flash =
          "<div class=\"flash good\"><p><b>New orchestrator key</b> (valid until " + format_time(issued.key.expires) +
          "). Copy it now: it is not shown again.</p><pre class=\"code\">" + html_esc(issued.text) +
          "</pre><p>Give it to the orchestrator in its environment, for example:</p><pre class=\"code\">"
          "HS_ORCH_KEY=" + html_esc(issued.text) + "</pre><p>It then issues worker keys: <code>hs key issue FEAT-12 "
          "--title \"...\" [--scope \"src/**\"] [--hours 8]</code>.</p></div>";
      return access_page(*site->access, project, user, flash);
    });
  });
  srv.Post(P + "/revoke", [=](const httplib::Request& req, httplib::Response& res) {
    run(req, res, [&](const std::string& project, const std::string& user) {
      need_user(user);
      site->access->revoke(project, req.get_param_value("id"), user, nullptr);
      res.set_redirect("/ui/p/" + url_encode(project) + "/access", 303);
      return std::string();
    });
  });
  srv.Post(P + "/require", [=](const httplib::Request& req, httplib::Response& res) {
    run(req, res, [&](const std::string& project, const std::string& user) {
      need_user(user);
      site->access->set_required(project, req.get_param_value("on") == "1");
      res.set_redirect("/ui/p/" + url_encode(project) + "/access", 303);
      return std::string();
    });
  });
}

}  // namespace hs
