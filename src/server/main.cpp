// hshub: the hashstash hub server. Hosts projects, serves the sync protocol,
// shared locks, and the MCP tools over HTTP, and runs background jobs
// (publishing to git) after pushes.
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

#include "hub/backend.h"
#include "hub/service.h"
#include "hub/workspace.h"
#include "mcp/server.h"
#include "ops/publish.h"
#include "ops/repo.h"
#include "web/ui.h"

using namespace hs;
using json = nlohmann::json;

static const char* kUsage = R"(hshub - hashstash hub server

  hshub serve   [--root dir] [--host 127.0.0.1] [--port 7420] [--verify-on-push] [--token secret]
                [--review-token secret] [--cache dir] [--force-cache]
  hshub create  <project> [--root dir]
  hshub list    [--root dir]
  hshub publish <project> [--branch main] [--root dir]

Clients use http://<host>:<port>/<project> as their remote:
  hs remote add origin http://localhost:7420/myproject
Agents can use the hub's MCP endpoint directly: http://<host>:<port>/mcp/<project>
Review pages for people: http://<host>:<port>/ui
With --token (or HS_HUB_TOKEN), clients must send the same token (HS_TOKEN), and the
review pages ask for it once per browser.
With --review-token (or HS_HUB_REVIEW_TOKEN), recording a person's review on the pages
needs that token too, so only people you give it to can mark commits as human reviewed.
Publishing is set per project in <root>/<project>/.hs/config.toml under [publish].
--cache sets where workspaces for non-SQLite backends are kept; --force-cache uses such a
cached workspace even with SQLite, to run exactly the way a scaled hub runs.
Agents get their events (requests, granted locks, finished requests) from
POST /api/p/<project>/coord {"method":"events","agent":..,"after":..,"wait":seconds},
which waits up to 60 s for one, or as webhooks (hs subscribe <url>).
)";

namespace {

std::map<std::string, std::string> opts(int argc, char** argv, std::vector<std::string>& pos) {
  std::map<std::string, std::string> o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--verify-on-push" || a == "--force-cache") o[a] = "1";
    else if (starts_with(a, "--") && i + 1 < argc) o[a] = argv[++i];
    else pos.push_back(a);
  }
  return o;
}

std::string get(const std::map<std::string, std::string>& o, const std::string& k, const std::string& d) {
  auto it = o.find(k);
  return it == o.end() ? d : it->second;
}

int serve(const std::map<std::string, std::string>& o) {
  std::string root = get(o, "--root", "hub-data");
  std::string host = get(o, "--host", "127.0.0.1");
  int port = std::stoi(get(o, "--port", "7420"));
  std::string token = get(o, "--token", std::getenv("HS_HUB_TOKEN") ? std::getenv("HS_HUB_TOKEN") : "");
  auto backend = make_sqlite_backend(root);
  HubOptions hopt;
  hopt.verify_on_push = o.count("--verify-on-push") > 0;
  hopt.cache_dir = get(o, "--cache", "");
  hopt.force_cache = o.count("--force-cache") > 0;
  HubService service(*backend, hopt);
  // Serialises requests in this process (SQLite: one writer at a time). All
  // state is in the backend, so more hub servers can run side by side.
  std::mutex mu;

  httplib::Server srv;
  srv.set_payload_max_length(512ull * 1024 * 1024);
  auto authorized = [&](const httplib::Request& req, httplib::Response& res) {
    if (token.empty()) return true;
    if (req.get_header_value("Authorization") == "Bearer " + token) return true;
    res.status = 401;
    res.set_content(R"({"error":"unauthorized"})", "application/json");
    return false;
  };

  srv.Get("/", [&](const httplib::Request&, httplib::Response& res) { res.set_redirect("/ui"); });

  // Review pages. With a hub token, a browser enters it once (kept in a cookie).
  std::string review_token =
      get(o, "--review-token", std::getenv("HS_HUB_REVIEW_TOKEN") ? std::getenv("HS_HUB_REVIEW_TOKEN") : "");
  if (!token.empty()) {
    srv.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
      if (!starts_with(req.path, "/ui") || req.path == "/ui/login") return httplib::Server::HandlerResponse::Unhandled;
      if (req.get_header_value("Cookie").find("hs_token=" + token) != std::string::npos)
        return httplib::Server::HandlerResponse::Unhandled;
      res.set_content("<!doctype html><meta charset=utf-8><title>hashstash hub</title>"
                      "<body style=\"font:16px system-ui;margin:40px\"><h1>hashstash hub</h1>"
                      "<form method=post action=/ui/login>Hub token <input type=password name=token autofocus> "
                      "<button>Open</button></form>",
                      "text/html; charset=utf-8");
      return httplib::Server::HandlerResponse::Handled;
    });
    srv.Post("/ui/login", [&](const httplib::Request& req, httplib::Response& res) {
      if (req.get_param_value("token") == token)
        res.set_header("Set-Cookie", "hs_token=" + token + "; Path=/; HttpOnly; SameSite=Strict");
      res.set_redirect("/ui", 303);
    });
  }
  UiSite site;
  site.where = "hub";
  site.projects = [&] { return backend->projects(); };
  site.open = [&](const std::string& name) -> Repo* {
    Workspace* ws = service.workspace(name);
    return ws ? &ws->repo() : nullptr;
  };
  site.changed = [&](const std::string& name) {
    if (Workspace* ws = service.workspace(name)) ws->save("review pages");
  };
  site.mu = &mu;
  site.review_token = review_token;
  mount_ui(srv, site);
  srv.Get("/api/projects", [&](const httplib::Request& req, httplib::Response& res) {
    if (!authorized(req, res)) return;
    std::lock_guard<std::mutex> g(mu);
    res.set_content(json{{"projects", backend->projects()}}.dump(), "application/json");
  });
  srv.Post(R"(/api/p/([A-Za-z0-9._-]+)/([a-z_]+))", [&](const httplib::Request& req, httplib::Response& res) {
    if (!authorized(req, res)) return;
    try {
      json body = req.body.empty() ? json::object() : json::parse(req.body);
      std::string project = req.matches[1], op = req.matches[2];
      // Long poll for events: ask, and if there is nothing yet, ask again
      // every half second (without holding the lock) until wait runs out.
      int wait = op == "coord" && body.value("method", "") == "events" ? std::min(body.value("wait", 0), 60) : 0;
      auto until = std::chrono::steady_clock::now() + std::chrono::seconds(wait);
      json out;
      for (;;) {
        {
          std::lock_guard<std::mutex> g(mu);
          out = service.handle(project, op, body);
        }
        if (!wait || !out["events"].empty() || std::chrono::steady_clock::now() >= until) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
      res.set_content(out.dump(), "application/json");
    } catch (std::exception& e) {
      res.status = 400;
      res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
  });
  // MCP over HTTP (JSON responses). The hub's tools act on the project's
  // working state there: edits made through it are like editing on the website.
  srv.Post(R"(/mcp/([A-Za-z0-9._-]+))", [&](const httplib::Request& req, httplib::Response& res) {
    if (!authorized(req, res)) return;
    std::lock_guard<std::mutex> g(mu);
    try {
      std::string project = req.matches[1];
      Workspace* ws = service.workspace(project);
      if (!ws) throw std::runtime_error("no project '" + project + "'");
      Repo& repo = ws->repo();
      json msg = json::parse(req.body);
      // Sessions live in the backend (who the agent is), so any hub server
      // can answer any request of a session.
      std::string sid = req.get_header_value("Mcp-Session-Id");
      json session = sid.empty() ? json(nullptr) : backend->load_session(project + "/" + sid);
      if (msg.value("method", "") == "initialize" || session.is_null()) {
        sid = new_uuid();
        session = {{"agent", ""}};
      }
      repo.set_agent(session.value("agent", "").empty() ? "hub" : session.value("agent", ""));
      json out;
      try {
        out = McpServer(repo).handle(msg);
        session["agent"] = repo.agent();
        backend->save_session(project + "/" + sid, session, now_seconds() + 7 * 24 * 3600);
        ws->save(repo.agent());
      } catch (...) {
        repo.set_agent("hub");
        throw;
      }
      repo.set_agent("hub");
      res.set_header("Mcp-Session-Id", sid);
      if (out.is_null()) {
        res.status = 202;
        return;
      }
      res.set_content(out.dump(), "application/json");
    } catch (std::exception& e) {
      res.status = 400;
      res.set_content(json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32603}, {"message", e.what()}}}}.dump(),
                      "application/json");
    }
  });

  // Background jobs (publishing) run off the request path.
  std::atomic<bool> running{true};
  std::thread worker([&] {
    while (running) {
      {
        std::lock_guard<std::mutex> g(mu);
        int n = service.run_jobs();
        if (n) std::cout << "ran " << n << " background job(s)" << std::endl;
        service.deliver_webhooks();
      }
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  });

  std::cout << "hshub serving " << to_generic(fs::absolute(from_utf8(root))) << " on http://" << host << ":" << port
            << (token.empty() ? "" : " (token required)") << "\n  review pages: http://" << host << ":" << port
            << "/ui" << std::endl;
  bool ok = srv.listen(host, port);
  running = false;
  worker.join();
  if (!ok) {
    std::cerr << "hshub: cannot listen on " << host << ":" << port << "\n";
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> pos;
  auto o = opts(argc, argv, pos);
  if (pos.empty() || pos[0] == "help") {
    std::cout << kUsage;
    return 0;
  }
  try {
    std::string root = get(o, "--root", "hub-data");
    if (pos[0] == "serve") return serve(o);
    auto backend = make_sqlite_backend(root);
    if (pos[0] == "list") {
      for (auto& p : backend->projects()) std::cout << p << "\n";
      return 0;
    }
    if (pos.size() < 2) throw std::runtime_error("missing project name");
    if (pos[0] == "create") {
      backend->project(pos[1], true);
      std::cout << "Created project " << pos[1] << " in " << root << "\n";
      return 0;
    }
    if (pos[0] == "publish") {
      HubOptions hopt;
      hopt.cache_dir = get(o, "--cache", "");
      hopt.force_cache = o.count("--force-cache") > 0;
      HubService service(*backend, hopt);
      Workspace* ws = service.workspace(pos[1]);
      if (!ws) throw std::runtime_error("no project " + pos[1]);
      std::cout << publish(ws->repo(), get(o, "--branch", "")).dump(2) << "\n";
      return 0;
    }
    std::cerr << kUsage;
    return 2;
  } catch (std::exception& e) {
    std::cerr << "hshub: " << e.what() << "\n";
    return 1;
  }
}
