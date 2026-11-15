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

class SqliteProject : public ProjectStore, public LocalRepoAccess {
 public:
  SqliteProject(std::string name, std::unique_ptr<Repo> repo) : name_(std::move(name)), repo_(std::move(repo)) {}
  std::string name() const override { return name_; }
  Repo* local_repo() override { return repo_.get(); }

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
    bool ok = false;
    store().transaction([&] {
      std::string cur = store().ref("branch/" + branch).value_or("");
      if (cur != old_head) return;
      store().set_ref("branch/" + branch, new_head);
      ok = true;
    });
    return ok;
  }

  std::vector<HubLock> locks() override {
    Stmt st(store().db(), "SELECT target, name, holder, expires FROM lock WHERE expires > ? ORDER BY name");
    st.bind(1, now_seconds());
    std::vector<HubLock> out;
    while (st.step()) out.push_back({st.text(0), st.text(1), st.text(2), st.integer(3)});
    return out;
  }
  std::optional<HubLock> acquire(const HubLock& l) override {
    std::optional<HubLock> blocked;
    store().transaction([&] {
      Stmt st(store().db(), "SELECT target, name, holder, expires FROM lock WHERE target = ? AND expires > ?");
      st.bind(1, l.target).bind(2, now_seconds());
      if (st.step() && st.text(2) != l.holder) {
        blocked = HubLock{st.text(0), st.text(1), st.text(2), st.integer(3)};
        return;
      }
      Stmt up(store().db(),
              "INSERT INTO lock(target, name, holder, acquired, expires) VALUES (?, ?, ?, ?, ?) "
              "ON CONFLICT(target) DO UPDATE SET name = excluded.name, holder = excluded.holder, "
              "expires = excluded.expires");
      up.bind(1, l.target).bind(2, l.name).bind(3, l.holder).bind(4, now_seconds()).bind(5, l.expires).run();
    });
    return blocked;
  }
  bool release(const std::string& target, const std::string& holder, bool force) override {
    Stmt st(store().db(), force ? "DELETE FROM lock WHERE target = ?"
                                : "DELETE FROM lock WHERE target = ? AND holder = ?");
    st.bind(1, target);
    if (!force) st.bind(2, holder);
    st.run();
    return sqlite3_changes(store().db()) > 0;
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

  void begin() override { store().exec("BEGIN IMMEDIATE"); }
  void commit() override { store().exec("COMMIT"); }
  void rollback() override { store().exec("ROLLBACK"); }

 private:
  Store& store() { return repo_->store(); }
  std::string name_;
  std::unique_ptr<Repo> repo_;
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

 private:
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
