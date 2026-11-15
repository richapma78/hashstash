// SQLite hub backend: each hosted project is an ordinary hashstash project
// (<root>/<name>/.hs/store.db), so the hub can also index, query and render
// it with the same code as the client. Jobs live in <root>/hub.db.
#include <sqlite3.h>

#include <mutex>

#include "hub/backend.h"
#include "hub/service.h"
#include "ops/repo.h"
#include "ops/review.h"

namespace hs {

namespace {

class SqliteProject : public ProjectStore {
 public:
  SqliteProject(std::string name, std::unique_ptr<Repo> repo)
      : name_(std::move(name)), repo_(std::move(repo)), coord_(sql_coordination(repo_->store(), repo_->config().lock_minutes)) {}
  std::string name() const override { return name_; }
  Repo* native_repo() override { return repo_.get(); }

  bool has(const Hash& h) override { return store().has(h); }
  std::optional<Object> get(const Hash& h) override { return store().get(h); }
  void put(const Hash& h, const Object& o) override {
    Stmt st(store().db(), "INSERT OR IGNORE INTO object(hash, kind, body) VALUES (?, ?, ?)");
    st.bind(1, h).bind(2, o.kind).bind_blob(3, o.body).run();
  }

  std::map<std::string, Hash> branches() override {
    std::map<std::string, Hash> out;
    for (auto& [k, v] : store().refs_with_prefix("branch/"))
      if (!v.empty()) out[k.substr(7)] = v;
    return out;
  }
  bool cas_branch(const std::string& branch, const Hash& old_head, const Hash& new_head) override {
    return cas_ref("branch/" + branch, old_head, new_head);
  }
  std::string ref(const std::string& name) override { return store().ref(name).value_or(""); }
  bool cas_ref(const std::string& name, const std::string& old_value, const std::string& new_value) override {
    bool ok = false;
    store().transaction([&] {
      if (store().ref(name).value_or("") != old_value) return;
      store().set_ref(name, new_value);
      ok = true;
    });
    return ok;
  }

  Coordination& coord() override { return *coord_; }
  std::string config_text() override {
    fs::path p = repo_->hs_dir() / "config.toml";
    return fs::exists(p) ? read_file(p) : "";
  }

  void set_verification(const Hash& ns, const std::string& status, const std::string& report) override {
    Stmt st(store().db(),
            "INSERT OR REPLACE INTO verification(ns, status, time, agent, report) VALUES (?, ?, ?, 'hub', ?)");
    st.bind(1, ns).bind(2, status).bind(3, now_seconds()).bind(4, report).run();
  }
  std::string verification(const Hash& ns) override {
    Stmt st(store().db(), "SELECT status FROM verification WHERE ns = ?");
    st.bind(1, ns);
    return st.step() ? st.text(0) : "";
  }

  std::string verification_report(const Hash& ns) override {
    Stmt st(store().db(), "SELECT report FROM verification WHERE ns = ?");
    st.bind(1, ns);
    return st.step() ? st.text(0) : "";
  }
  void put_review(const nlohmann::json& r) override { store_review(store(), review_from_json(r), true); }
  nlohmann::json reviews_after(long long seq) override { return hs::reviews_after(store(), seq); }
  void log_push(const std::string& branch, const Hash& old_head, const Hash& new_head,
                const std::string& agent) override {
    hs::log_push(store(), branch, old_head, new_head, agent);
  }
  nlohmann::json pushes_after(long long id) override {
    Stmt st(store().db(),
            "SELECT id, branch, old_head, new_head, agent, time FROM push_log WHERE id > ? ORDER BY id LIMIT 500");
    st.bind(1, id);
    nlohmann::json out = nlohmann::json::array();
    long long last = id;
    while (st.step()) {
      last = st.integer(0);
      out.push_back({{"id", last}, {"branch", st.text(1)}, {"old", st.text(2)}, {"new", st.text(3)},
                     {"agent", st.text(4)}, {"time", st.integer(5)}});
    }
    return {{"pushes", out}, {"last", last}};
  }

  void begin() override { store().exec("BEGIN IMMEDIATE"); }
  void commit() override { store().exec("COMMIT"); }
  void rollback() override { store().exec("ROLLBACK"); }

 private:
  Store& store() { return repo_->store(); }
  std::string name_;
  std::unique_ptr<Repo> repo_;
  std::unique_ptr<Coordination> coord_;
};

bool valid_name(const std::string& n) {
  if (n.empty() || n.size() > 100 || n[0] == '.') return false;
  for (char c : n)
    if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
  return true;
}

class SqliteBackend : public HubBackend {
 public:
  explicit SqliteBackend(std::string root) : root_(fs::absolute(from_utf8(root))) {
    fs::create_directories(root_);
    jobs_ = std::make_unique<Store>(to_generic(root_ / "hub.db"));
    jobs_->exec(
        "CREATE TABLE IF NOT EXISTS job (id INTEGER PRIMARY KEY AUTOINCREMENT, project TEXT NOT NULL, "
        "kind TEXT NOT NULL, arg TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'queued', "
        "created INTEGER NOT NULL, result TEXT NOT NULL DEFAULT '')");
    jobs_->exec(
        "CREATE TABLE IF NOT EXISTS mcp_session (id TEXT PRIMARY KEY, data TEXT NOT NULL, expires INTEGER NOT NULL)");
    jobs_->exec("CREATE TABLE IF NOT EXISTS hub_user (name TEXT PRIMARY KEY, data TEXT NOT NULL)");
    jobs_->exec(
        "CREATE TABLE IF NOT EXISTS access_key (id TEXT PRIMARY KEY, project TEXT NOT NULL, data TEXT NOT NULL)");
    jobs_->exec("CREATE TABLE IF NOT EXISTS hub_setting (name TEXT PRIMARY KEY, value TEXT NOT NULL)");
  }

  std::vector<std::string> projects() override {
    std::vector<std::string> out;
    for (auto& e : fs::directory_iterator(root_))
      if (e.is_directory() && fs::exists(e.path() / ".hs")) out.push_back(to_generic(e.path().filename()));
    return out;
  }

  std::shared_ptr<ProjectStore> project(const std::string& name, bool create) override {
    if (!valid_name(name)) throw std::runtime_error("invalid project name: " + name);
    std::lock_guard<std::mutex> g(mu_);
    auto it = open_.find(name);
    if (it != open_.end()) return it->second;
    fs::path dir = root_ / from_utf8(name);
    std::unique_ptr<Repo> repo;
    if (fs::exists(dir / ".hs")) repo = Repo::open(dir);
    else if (create) {
      fs::create_directories(dir);
      repo = Repo::init(dir, name);
    } else {
      return nullptr;
    }
    repo->set_agent("hub");
    auto p = std::make_shared<SqliteProject>(name, std::move(repo));
    open_[name] = p;
    return p;
  }

  void enqueue(const HubJob& j) override {
    std::lock_guard<std::mutex> g(mu_);
    Stmt st(jobs_->db(), "INSERT INTO job(project, kind, arg, created) VALUES (?, ?, ?, ?)");
    st.bind(1, j.project).bind(2, j.kind).bind(3, j.arg).bind(4, now_seconds()).run();
  }
  std::optional<HubJob> next_job() override {
    std::lock_guard<std::mutex> g(mu_);
    std::optional<HubJob> out;
    jobs_->transaction([&] {
      Stmt st(jobs_->db(), "SELECT id, project, kind, arg FROM job WHERE state = 'queued' ORDER BY id LIMIT 1");
      if (!st.step()) return;
      out = HubJob{st.integer(0), st.text(1), st.text(2), st.text(3)};
      Stmt up(jobs_->db(), "UPDATE job SET state = 'running' WHERE id = ?");
      up.bind(1, out->id).run();
    });
    return out;
  }
  void finish_job(long long id, const std::string& result) override {
    std::lock_guard<std::mutex> g(mu_);
    Stmt st(jobs_->db(), "UPDATE job SET state = 'done', result = ? WHERE id = ?");
    st.bind(1, result).bind(2, id).run();
  }

  void save_session(const std::string& id, const nlohmann::json& data, long long expires) override {
    std::lock_guard<std::mutex> g(mu_);
    Stmt st(jobs_->db(), "INSERT OR REPLACE INTO mcp_session(id, data, expires) VALUES (?, ?, ?)");
    st.bind(1, id).bind(2, data.dump()).bind(3, expires).run();
    Stmt old(jobs_->db(), "DELETE FROM mcp_session WHERE expires < ?");
    old.bind(1, now_seconds()).run();
  }
  void drop_session(const std::string& id) override {
    std::lock_guard<std::mutex> g(mu_);
    Stmt st(jobs_->db(), "DELETE FROM mcp_session WHERE id = ?");
    st.bind(1, id).run();
  }

  void put_user(const nlohmann::json& u) override {
    std::lock_guard<std::mutex> g(mu_);
    Stmt st(jobs_->db(), "INSERT OR REPLACE INTO hub_user(name, data) VALUES (?, ?)");
    st.bind(1, u.at("name").get<std::string>()).bind(2, u.dump()).run();
  }
  nlohmann::json user(const std::string& name) override { return doc("SELECT data FROM hub_user WHERE name = ?", name); }
  int user_count() override {
    std::lock_guard<std::mutex> g(mu_);
    Stmt st(jobs_->db(), "SELECT COUNT(*) FROM hub_user");
    return st.step() ? (int)st.integer(0) : 0;
  }
  void put_key(const nlohmann::json& k) override {
    std::lock_guard<std::mutex> g(mu_);
    Stmt st(jobs_->db(), "INSERT OR REPLACE INTO access_key(id, project, data) VALUES (?, ?, ?)");
    st.bind(1, k.at("id").get<std::string>()).bind(2, k.value("project", "")).bind(3, k.dump()).run();
  }
  nlohmann::json key(const std::string& id) override { return doc("SELECT data FROM access_key WHERE id = ?", id); }
  nlohmann::json keys(const std::string& project) override {
    std::lock_guard<std::mutex> g(mu_);
    Stmt st(jobs_->db(), "SELECT data FROM access_key WHERE project = ? ORDER BY rowid");
    st.bind(1, project);
    nlohmann::json out = nlohmann::json::array();
    while (st.step()) out.push_back(nlohmann::json::parse(st.text(0)));
    return out;
  }
  void set_setting(const std::string& name, const std::string& value) override {
    std::lock_guard<std::mutex> g(mu_);
    Stmt st(jobs_->db(), "INSERT OR REPLACE INTO hub_setting(name, value) VALUES (?, ?)");
    st.bind(1, name).bind(2, value).run();
  }
  std::string setting(const std::string& name) override {
    std::lock_guard<std::mutex> g(mu_);
    Stmt st(jobs_->db(), "SELECT value FROM hub_setting WHERE name = ?");
    st.bind(1, name);
    return st.step() ? st.text(0) : "";
  }

  nlohmann::json load_session(const std::string& id) override {
    std::lock_guard<std::mutex> g(mu_);
    Stmt st(jobs_->db(), "SELECT data FROM mcp_session WHERE id = ? AND expires >= ?");
    st.bind(1, id).bind(2, now_seconds());
    if (!st.step()) return nullptr;
    try {
      return nlohmann::json::parse(st.text(0));
    } catch (...) {
      return nullptr;
    }
  }

 private:
  nlohmann::json doc(const char* sql, const std::string& key) {
    std::lock_guard<std::mutex> g(mu_);
    Stmt st(jobs_->db(), sql);
    st.bind(1, key);
    if (!st.step()) return nullptr;
    return nlohmann::json::parse(st.text(0));
  }

  fs::path root_;
  std::unique_ptr<Store> jobs_;
  std::mutex mu_;
  std::map<std::string, std::shared_ptr<ProjectStore>> open_;
};

}  // namespace

std::unique_ptr<HubBackend> make_sqlite_backend(const std::string& root) {
  return std::make_unique<SqliteBackend>(root);
}

}  // namespace hs
