#include "ops/coord.h"

#include <sqlite3.h>

#include <chrono>
#include <set>
#include <stdexcept>
#include <thread>

#include "core/util.h"
#include "ops/sync.h"

namespace hs {

using json = nlohmann::json;

std::vector<Event> Coordination::events_wait(const std::string& agent, long long after, int limit, int wait_seconds) {
  auto until = std::chrono::steady_clock::now() + std::chrono::seconds(wait_seconds);
  for (;;) {
    auto out = events(agent, after, limit);
    if (!out.empty() || std::chrono::steady_clock::now() >= until) return out;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}

int default_priority(const std::string& kind) {
  if (kind == "bug") return 2;
  if (kind == "chore") return 0;
  return 1;
}

// ---- JSON forms (the hub protocol) -------------------------------------------

json LockRec::to_json() const {
  return {{"target", target}, {"name", name}, {"holder", holder}, {"work", work}, {"acquired", acquired}, {"expires", expires}};
}
LockRec LockRec::from_json(const json& j) {
  return {j.value("target", ""), j.value("name", ""), j.value("holder", ""), j.value("work", ""),
          j.value("acquired", 0LL), j.value("expires", 0LL)};
}

json WorkItem::to_json() const {
  return {{"id", id}, {"kind", kind}, {"title", title}, {"owner", owner}, {"link", link}, {"status", status},
          {"priority", priority}, {"created", created}, {"closed", closed}};
}
WorkItem WorkItem::from_json(const json& j) {
  WorkItem w;
  w.id = j.value("id", "");
  w.kind = j.value("kind", "");
  w.title = j.value("title", "");
  w.owner = j.value("owner", "");
  w.link = j.value("link", "");
  w.status = j.value("status", "");
  w.priority = j.value("priority", -1);
  w.created = j.value("created", 0LL);
  w.closed = j.value("closed", 0LL);
  return w;
}

json Request::to_json() const {
  return {{"id", id}, {"kind", kind}, {"target", target}, {"name", name}, {"requester", requester},
          {"requester_work", requester_work}, {"holder", holder}, {"holder_work", holder_work}, {"text", text},
          {"test_file", test_file}, {"test_code", test_code}, {"status", status}, {"commit", commit}, {"note", note},
          {"priority", priority}, {"created", created}, {"updated", updated}, {"context", context}};
}
Request Request::from_json(const json& j) {
  Request r;
  r.id = j.value("id", 0LL);
  r.kind = j.value("kind", "change");
  r.target = j.value("target", "");
  r.name = j.value("name", "");
  r.requester = j.value("requester", "");
  r.requester_work = j.value("requester_work", "");
  r.holder = j.value("holder", "");
  r.holder_work = j.value("holder_work", "");
  r.text = j.value("text", "");
  r.test_file = j.value("test_file", "");
  r.test_code = j.value("test_code", "");
  r.status = j.value("status", "open");
  r.commit = j.value("commit", "");
  r.note = j.value("note", "");
  r.priority = j.value("priority", -1);
  if (j.contains("context") && j["context"].is_object()) r.context = j["context"];
  r.created = j.value("created", 0LL);
  r.updated = j.value("updated", 0LL);
  return r;
}

json Event::to_json() const {
  return {{"seq", seq}, {"agent", agent}, {"work", work}, {"kind", kind}, {"data", data}, {"time", time}};
}
Event Event::from_json(const json& j) {
  return {j.value("seq", 0LL), j.value("agent", ""), j.value("work", ""), j.value("kind", ""),
          j.value("data", json::object()), j.value("time", 0LL)};
}

json Subscription::to_json() const { return {{"id", id}, {"agent", agent}, {"url", url}, {"after", after}}; }
Subscription Subscription::from_json(const json& j) {
  return {j.value("id", 0LL), j.value("agent", ""), j.value("url", ""), j.value("after", 0LL)};
}

json RequestFilter::to_json() const {
  return {{"agent", agent}, {"role", role}, {"target", target}, {"open_only", open_only}};
}
RequestFilter RequestFilter::from_json(const json& j) {
  return {j.value("agent", ""), j.value("role", ""), j.value("target", ""), j.value("open_only", true)};
}

namespace {

// ---- SQL implementation --------------------------------------------------------

const char* kRequestCols =
    "id, kind, target, name, requester, requester_work, holder, holder_work, text, test_file, test_code, "
    "status, commit_hash, note, priority, created, updated, context";

Request read_request(Stmt& st) {
  Request r;
  r.id = st.integer(0);
  r.kind = st.text(1);
  r.target = st.text(2);
  r.name = st.text(3);
  r.requester = st.text(4);
  r.requester_work = st.text(5);
  r.holder = st.text(6);
  r.holder_work = st.text(7);
  r.text = st.text(8);
  r.test_file = st.text(9);
  r.test_code = st.text(10);
  r.status = st.text(11);
  r.commit = st.text(12);
  r.note = st.text(13);
  r.priority = (int)st.integer(14);
  r.created = st.integer(15);
  r.updated = st.integer(16);
  try {
    r.context = json::parse(st.text(17));
  } catch (...) {
  }
  if (!r.context.is_object()) r.context = json::object();
  return r;
}

WorkItem read_work(Stmt& st) {
  WorkItem w;
  w.id = st.text(0);
  w.kind = st.text(1);
  w.title = st.text(2);
  w.owner = st.text(3);
  w.priority = (int)st.integer(4);
  w.link = st.text(5);
  w.status = st.text(6);
  w.created = st.integer(7);
  w.closed = st.integer(8);
  return w;
}

class SqlCoordination : public Coordination {
 public:
  SqlCoordination(Store& s, int lease) : s_(s), lease_(lease > 0 ? lease : 15) {}

  std::vector<LockRec> locks() override {
    sweep();
    Stmt st(s_.db(), "SELECT target, name, holder, work, acquired, expires FROM lock WHERE expires > ? ORDER BY name");
    st.bind(1, now_seconds());
    std::vector<LockRec> out;
    while (st.step()) out.push_back({st.text(0), st.text(1), st.text(2), st.text(3), st.integer(4), st.integer(5)});
    return out;
  }

  std::optional<LockRec> acquire(const LockRec& l) override {
    std::optional<LockRec> blocked;
    s_.transaction([&] {
      sweep();
      if (auto cur = lock_on(l.target); cur && cur->holder != l.holder) {
        blocked = cur;
        return;
      }
      long long now = now_seconds();
      Stmt up(s_.db(),
              "INSERT INTO lock(target, name, holder, acquired, expires, work) VALUES (?, ?, ?, ?, ?, ?) "
              "ON CONFLICT(target) DO UPDATE SET name = excluded.name, holder = excluded.holder, "
              "expires = excluded.expires, work = excluded.work, origin = ''");
      up.bind(1, l.target).bind(2, l.name).bind(3, l.holder).bind(4, l.acquired ? l.acquired : now)
          .bind(5, l.expires ? l.expires : now + 60LL * lease_).bind(6, l.work).run();
    });
    return blocked;
  }

  bool release(const std::string& target, const std::string& holder, bool force) override {
    bool done = false;
    s_.transaction([&] {
      Stmt st(s_.db(), force ? "DELETE FROM lock WHERE target = ?" : "DELETE FROM lock WHERE target = ? AND holder = ?");
      st.bind(1, target);
      if (!force) st.bind(2, holder);
      st.run();
      done = changes() > 0;
      if (done) hand_over(target);
    });
    return done;
  }

  void sync_locks(const std::string& holder, const std::vector<LockRec>& keep) override {
    s_.transaction([&] {
      std::set<std::string> kept;
      for (auto& l : keep) kept.insert(l.target);
      for (auto& l : locks())
        if (l.holder == holder && !kept.count(l.target)) release(l.target, holder, false);
      for (auto& l : keep) {
        LockRec r = l;
        r.holder = holder;
        acquire(r);  // a lock someone else took meanwhile stays theirs
      }
    });
  }

  WorkItem put_work(const WorkItem& in) override {
    if (in.id.empty()) throw std::runtime_error("a work item needs an id (e.g. FEAT-12 or BUG-7)");
    WorkItem w;
    s_.transaction([&] {
      auto cur = work(in.id);
      long long now = now_seconds();
      if (!cur) {
        w = in;
        if (w.kind.empty()) w.kind = "feature";
        if (w.kind != "feature" && w.kind != "bug" && w.kind != "chore")
          throw std::runtime_error("work item kind must be feature, bug or chore");
        if (w.priority < 0) w.priority = default_priority(w.kind);
        if (w.status.empty()) w.status = "open";
        w.created = now;
        Stmt st(s_.db(),
                "INSERT INTO work_item(id, kind, title, owner, priority, link, status, created, closed) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0)");
        st.bind(1, w.id).bind(2, w.kind).bind(3, w.title).bind(4, w.owner).bind(5, (long long)w.priority)
            .bind(6, w.link).bind(7, w.status).bind(8, now).run();
        return;
      }
      w = *cur;
      if (!in.kind.empty()) w.kind = in.kind;
      if (!in.title.empty()) w.title = in.title;
      if (!in.owner.empty()) w.owner = in.owner;
      if (!in.link.empty()) w.link = in.link;
      if (in.priority >= 0) w.priority = in.priority;
      if (!in.status.empty() && in.status != w.status) {
        w.status = in.status;
        w.closed = w.status == "open" ? 0 : now;
      }
      Stmt st(s_.db(),
              "UPDATE work_item SET kind = ?, title = ?, owner = ?, priority = ?, link = ?, status = ?, closed = ? "
              "WHERE id = ?");
      st.bind(1, w.kind).bind(2, w.title).bind(3, w.owner).bind(4, (long long)w.priority).bind(5, w.link)
          .bind(6, w.status).bind(7, w.closed).bind(8, w.id).run();
    });
    return w;
  }

  std::optional<WorkItem> work(const std::string& id) override {
    Stmt st(s_.db(), "SELECT id, kind, title, owner, priority, link, status, created, closed FROM work_item WHERE id = ?");
    st.bind(1, id);
    if (!st.step()) return std::nullopt;
    return read_work(st);
  }

  std::vector<WorkItem> works(bool include_closed) override {
    Stmt st(s_.db(), include_closed
                         ? "SELECT id, kind, title, owner, priority, link, status, created, closed FROM work_item "
                           "ORDER BY status = 'open' DESC, priority DESC, created"
                         : "SELECT id, kind, title, owner, priority, link, status, created, closed FROM work_item "
                           "WHERE status = 'open' ORDER BY priority DESC, created");
    std::vector<WorkItem> out;
    while (st.step()) out.push_back(read_work(st));
    return out;
  }

  Request add_request(const Request& in) override {
    if (in.kind != "change" && in.kind != "lock") throw std::runtime_error("request kind must be change or lock");
    if (in.requester.empty() || in.target.empty()) throw std::runtime_error("request needs a requester and a target");
    if (in.kind == "change" && trim(in.text).empty())
      throw std::runtime_error("a change request needs a description of the change you need");
    Request r = in;
    s_.transaction([&] {
      sweep();
      auto l = lock_on(r.target);
      if (!l) throw std::runtime_error(r.name + " is not locked: lock it and make the change yourself");
      if (l->holder == r.requester) throw std::runtime_error("you hold the lock on " + r.name + " yourself");
      // One open request per requester, definition and kind: a repeat updates it.
      {
        Stmt st(s_.db(), (std::string("SELECT ") + kRequestCols +
                          " FROM request WHERE target = ? AND requester = ? AND kind = ? AND status IN ('open','accepted')")
                             .c_str());
        st.bind(1, r.target).bind(2, r.requester).bind(3, r.kind);
        if (st.step()) {
          Request cur = read_request(st);
          if (!r.text.empty()) cur.text = r.text;
          if (!r.test_code.empty()) cur.test_code = r.test_code, cur.test_file = r.test_file;
          if (!r.context.empty()) cur.context = r.context;
          cur.updated = now_seconds();
          Stmt up(s_.db(),
                  "UPDATE request SET text = ?, test_file = ?, test_code = ?, context = ?, updated = ? WHERE id = ?");
          up.bind(1, cur.text).bind(2, cur.test_file).bind(3, cur.test_code).bind(4, cur.context.dump())
              .bind(5, cur.updated).bind(6, cur.id).run();
          r = cur;
          emit(cur.holder, cur.holder_work, "request_updated", {{"request", cur.to_json()}});
          return;
        }
      }
      r.holder = l->holder;
      r.holder_work = l->work;
      if (r.priority < 0) {
        auto w = r.requester_work.empty() ? std::nullopt : work(r.requester_work);
        r.priority = w ? w->priority : 1;
      }
      r.status = "open";
      r.commit.clear();
      r.created = r.updated = now_seconds();
      insert_request(r);
      emit(r.holder, r.holder_work, "request_new", {{"request", r.to_json()}});
    });
    return r;
  }

  std::optional<Request> request(long long id) override {
    Stmt st(s_.db(), (std::string("SELECT ") + kRequestCols + " FROM request WHERE id = ?").c_str());
    st.bind(1, id);
    if (!st.step()) return std::nullopt;
    return read_request(st);
  }

  std::vector<Request> requests(const RequestFilter& f) override {
    sweep();
    std::string sql = std::string("SELECT ") + kRequestCols + " FROM request WHERE 1";
    if (f.open_only) sql += " AND status IN ('open','accepted','resolved')";
    if (!f.target.empty()) sql += " AND target = ?1";
    if (!f.agent.empty()) {
      if (f.role == "holder") sql += " AND holder = ?2";
      else if (f.role == "requester") sql += " AND requester = ?2";
      else sql += " AND (holder = ?2 OR requester = ?2)";
    }
    sql += " ORDER BY priority DESC, id LIMIT 500";
    Stmt st(s_.db(), sql.c_str());
    if (!f.target.empty()) st.bind(1, f.target);
    if (!f.agent.empty()) st.bind(2, f.agent);
    std::vector<Request> out;
    while (st.step()) out.push_back(read_request(st));
    return out;
  }

  Request update_request(long long id, const std::string& action, const std::string& actor, const std::string& note,
                         const std::string& commit) override {
    Request r;
    s_.transaction([&] {
      auto cur = request(id);
      if (!cur) throw std::runtime_error("no request R-" + std::to_string(id));
      r = *cur;
      auto need_open = [&] {
        if (!r.is_open()) throw std::runtime_error("request R-" + std::to_string(id) + " is already " + r.status);
      };
      auto need_holder = [&] {
        if (actor != r.holder)
          throw std::runtime_error("request R-" + std::to_string(id) + " is for " + r.holder + ", not " + actor);
      };
      if (action == "accept") {
        need_open(), need_holder();
        set_status(r, "accepted", note, "");
        emit(r.requester, r.requester_work, "request_accepted", {{"request", r.to_json()}});
      } else if (action == "decline") {
        need_open(), need_holder();
        set_status(r, "declined", note, "");
        // The requester still needs the definition: queue it for the lock.
        Request q = r;
        q.kind = "lock";
        q.status = "open";
        q.note.clear();
        q.created = q.updated = now_seconds();
        bool queued = false;
        if (lock_on(r.target)) {
          insert_request(q);
          queued = true;
        }
        emit(r.requester, r.requester_work, "request_declined",
             {{"request", r.to_json()}, {"queued_for_lock", queued ? json(q.id) : json(nullptr)}});
        if (!queued) hand_over(r.target);
      } else if (action == "resolve") {
        need_open(), need_holder();
        if (commit.empty()) throw std::runtime_error("resolving a request needs the commit that made the change");
        set_status(r, "resolved", note, commit);
      } else if (action == "cancel") {
        need_open();
        if (actor != r.requester)
          throw std::runtime_error("only " + r.requester + " can cancel request R-" + std::to_string(id));
        set_status(r, "cancelled", note, "");
        emit(r.holder, r.holder_work, "request_cancelled", {{"request", r.to_json()}});
      } else {
        throw std::runtime_error("unknown request action " + action + " (accept, decline, resolve, cancel)");
      }
    });
    return r;
  }

  int land(const std::vector<std::string>& commits, const std::string& verified = "") override {
    int n = 0;
    if (commits.empty()) return 0;
    s_.transaction([&] {
      std::set<std::string> set(commits.begin(), commits.end());
      std::vector<Request> resolved;
      {
        Stmt st(s_.db(), (std::string("SELECT ") + kRequestCols + " FROM request WHERE status = 'resolved'").c_str());
        while (st.step()) resolved.push_back(read_request(st));
      }
      for (auto& r : resolved) {
        if (!set.count(r.commit)) continue;
        std::string resolved_in = r.commit, done_in = verified.empty() ? r.commit : verified;
        set_status(r, "done", r.note, done_in);
        emit(r.requester, r.requester_work, "request_done",
             {{"request", r.to_json()}, {"commit", done_in}, {"resolved_in", resolved_in}});
        ++n;
      }
    });
    return n;
  }

  std::vector<Event> events(const std::string& agent, long long after, int limit) override {
    sweep();
    Stmt st(s_.db(),
            "SELECT seq, agent, work, kind, data, time FROM event WHERE seq > ? AND (agent = ? OR "
            "(work <> '' AND work IN (SELECT id FROM work_item WHERE owner = ?))) ORDER BY seq LIMIT ?");
    st.bind(1, after).bind(2, agent).bind(3, agent).bind(4, (long long)(limit > 0 ? limit : 100));
    std::vector<Event> out;
    while (st.step()) {
      Event e{st.integer(0), st.text(1), st.text(2), st.text(3), json::object(), st.integer(5)};
      try {
        e.data = json::parse(st.text(4));
      } catch (...) {
      }
      out.push_back(e);
    }
    return out;
  }

  long long last_event() override {
    Stmt st(s_.db(), "SELECT COALESCE(MAX(seq), 0) FROM event");
    return st.step() ? st.integer(0) : 0;
  }

  Subscription subscribe(const std::string& agent, const std::string& url) override {
    if (!starts_with(url, "http://") && !starts_with(url, "https://"))
      throw std::runtime_error("a subscription URL must start with http:// or https://");
    Subscription sub;
    s_.transaction([&] {
      long long last = last_event();
      Stmt st(s_.db(), "INSERT OR IGNORE INTO subscription(agent, url, after, created) VALUES (?, ?, ?, ?)");
      st.bind(1, agent).bind(2, url).bind(3, last).bind(4, now_seconds()).run();
      Stmt q(s_.db(), "SELECT id, agent, url, after FROM subscription WHERE agent = ? AND url = ?");
      q.bind(1, agent).bind(2, url);
      if (q.step()) sub = {q.integer(0), q.text(1), q.text(2), q.integer(3)};
    });
    return sub;
  }

  bool unsubscribe(const std::string& agent, const std::string& url) override {
    Stmt st(s_.db(), "DELETE FROM subscription WHERE agent = ? AND url = ?");
    st.bind(1, agent).bind(2, url).run();
    return changes() > 0;
  }

  std::vector<Subscription> subscriptions() override {
    Stmt st(s_.db(), "SELECT id, agent, url, after FROM subscription ORDER BY id");
    std::vector<Subscription> out;
    while (st.step()) out.push_back({st.integer(0), st.text(1), st.text(2), st.integer(3)});
    return out;
  }

  void set_subscription_cursor(long long id, long long after) override {
    Stmt st(s_.db(), "UPDATE subscription SET after = ? WHERE id = ? AND after < ?");
    st.bind(1, after).bind(2, id).bind(3, after).run();
  }

 private:
  int changes() { return sqlite3_changes(s_.db()); }

  std::optional<LockRec> lock_on(const std::string& target) {
    Stmt st(s_.db(), "SELECT target, name, holder, work, acquired, expires FROM lock WHERE target = ? AND expires > ?");
    st.bind(1, target).bind(2, now_seconds());
    if (!st.step()) return std::nullopt;
    return LockRec{st.text(0), st.text(1), st.text(2), st.text(3), st.integer(4), st.integer(5)};
  }

  void insert_request(Request& r) {
    Stmt st(s_.db(),
            "INSERT INTO request(kind, target, name, requester, requester_work, holder, holder_work, text, test_file, "
            "test_code, status, commit_hash, note, priority, created, updated, context) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    st.bind(1, r.kind).bind(2, r.target).bind(3, r.name).bind(4, r.requester).bind(5, r.requester_work)
        .bind(6, r.holder).bind(7, r.holder_work).bind(8, r.text).bind(9, r.test_file).bind(10, r.test_code)
        .bind(11, r.status).bind(12, r.commit).bind(13, r.note).bind(14, (long long)r.priority).bind(15, r.created)
        .bind(16, r.updated).bind(17, r.context.dump()).run();
    Stmt id(s_.db(), "SELECT last_insert_rowid()");
    if (id.step()) r.id = id.integer(0);
  }

  void set_status(Request& r, const std::string& status, const std::string& note, const std::string& commit) {
    r.status = status;
    if (!note.empty()) r.note = note;
    if (!commit.empty()) r.commit = commit;
    r.updated = now_seconds();
    Stmt st(s_.db(), "UPDATE request SET status = ?, note = ?, commit_hash = ?, updated = ? WHERE id = ?");
    st.bind(1, r.status).bind(2, r.note).bind(3, r.commit).bind(4, r.updated).bind(5, r.id).run();
  }

  void emit(const std::string& agent, const std::string& work, const std::string& kind, const json& data) {
    Stmt st(s_.db(), "INSERT INTO event(agent, work, kind, data, time) VALUES (?, ?, ?, ?, ?)");
    st.bind(1, agent).bind(2, work).bind(3, kind).bind(4, data.dump()).bind(5, now_seconds()).run();
  }

  // The lock on target is free: give it to the first queued request.
  void hand_over(const std::string& target) {
    if (lock_on(target)) return;
    std::vector<Request> waiting;
    {
      Stmt st(s_.db(), (std::string("SELECT ") + kRequestCols +
                        " FROM request WHERE target = ? AND status IN ('open','accepted') ORDER BY priority DESC, id")
                           .c_str());
      st.bind(1, target);
      while (st.step()) waiting.push_back(read_request(st));
    }
    if (waiting.empty()) return;
    Request next = waiting.front();
    long long now = now_seconds();
    Stmt up(s_.db(),
            "INSERT INTO lock(target, name, holder, acquired, expires, work) VALUES (?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(target) DO UPDATE SET name = excluded.name, holder = excluded.holder, "
            "acquired = excluded.acquired, expires = excluded.expires, work = excluded.work, origin = ''");
    up.bind(1, target).bind(2, next.name).bind(3, next.requester).bind(4, now).bind(5, now + 60LL * lease_)
        .bind(6, next.requester_work).run();
    std::string previous = next.holder;
    set_status(next, next.kind == "lock" ? "granted" : "returned",
               next.kind == "lock" ? "" : previous + " released the lock without making the change", "");
    emit(next.requester, next.requester_work, "lock_granted",
         {{"request", next.to_json()},
          {"lock", LockRec{target, next.name, next.requester, next.requester_work, now, now + 60LL * lease_}.to_json()},
          {"note", next.kind == "lock" ? "your turn: the lock is yours"
                                       : "the holder released it without making your change; the lock is yours"}});
    // Everyone else now waits on the new holder.
    for (size_t i = 1; i < waiting.size(); ++i) {
      Request r = waiting[i];
      r.holder = next.requester;
      r.holder_work = next.requester_work;
      r.status = "open";
      r.updated = now;
      Stmt st(s_.db(), "UPDATE request SET holder = ?, holder_work = ?, status = 'open', updated = ? WHERE id = ?");
      st.bind(1, r.holder).bind(2, r.holder_work).bind(3, now).bind(4, r.id).run();
      emit(r.holder, r.holder_work, "request_new", {{"request", r.to_json()}, {"note", "passed on from " + previous}});
    }
  }

  // Expired leases that have requests queued: hand them over now.
  void sweep() {
    std::vector<std::string> targets;
    {
      Stmt st(s_.db(),
              "SELECT DISTINCT l.target FROM lock l JOIN request r ON r.target = l.target "
              "AND r.status IN ('open','accepted') WHERE l.expires <= ?");
      st.bind(1, now_seconds());
      while (st.step()) targets.push_back(st.text(0));
    }
    if (targets.empty()) return;
    s_.transaction([&] {
      for (auto& t : targets) {
        Stmt del(s_.db(), "DELETE FROM lock WHERE target = ? AND expires <= ?");
        del.bind(1, t).bind(2, now_seconds()).run();
        hand_over(t);
      }
    });
  }

  Store& s_;
  int lease_;
};

// ---- Hub implementation: every call is one hub request -----------------------

class HubCoordination : public Coordination {
 public:
  HubCoordination(HubTransport& t, std::unique_ptr<HubTransport> owned) : t_(t), owned_(std::move(owned)) {}

  std::vector<LockRec> locks() override {
    std::vector<LockRec> out;
    json res = call("locks", {});
    for (auto& l : res["locks"]) out.push_back(LockRec::from_json(l));
    return out;
  }
  std::optional<LockRec> acquire(const LockRec& l) override {
    json r = call("acquire", {{"lock", l.to_json()}});
    if (r.contains("blocked") && !r["blocked"].is_null()) return LockRec::from_json(r["blocked"]);
    return std::nullopt;
  }
  bool release(const std::string& target, const std::string& holder, bool force) override {
    return call("release", {{"target", target}, {"holder", holder}, {"force", force}}).value("released", false);
  }
  void sync_locks(const std::string& holder, const std::vector<LockRec>& keep) override {
    json k = json::array();
    for (auto& l : keep) k.push_back(l.to_json());
    call("sync_locks", {{"holder", holder}, {"keep", k}});
  }
  WorkItem put_work(const WorkItem& w) override { return WorkItem::from_json(call("put_work", {{"work", w.to_json()}})["work"]); }
  std::optional<WorkItem> work(const std::string& id) override {
    json r = call("work", {{"id", id}});
    if (r["work"].is_null()) return std::nullopt;
    return WorkItem::from_json(r["work"]);
  }
  std::vector<WorkItem> works(bool include_closed) override {
    std::vector<WorkItem> out;
    json res = call("works", {{"include_closed", include_closed}});
    for (auto& w : res["works"]) out.push_back(WorkItem::from_json(w));
    return out;
  }
  Request add_request(const Request& r) override {
    return Request::from_json(call("add_request", {{"request", r.to_json()}})["request"]);
  }
  std::optional<Request> request(long long id) override {
    json r = call("request", {{"id", id}});
    if (r["request"].is_null()) return std::nullopt;
    return Request::from_json(r["request"]);
  }
  std::vector<Request> requests(const RequestFilter& f) override {
    std::vector<Request> out;
    json res = call("requests", {{"filter", f.to_json()}});
    for (auto& r : res["requests"]) out.push_back(Request::from_json(r));
    return out;
  }
  Request update_request(long long id, const std::string& action, const std::string& actor, const std::string& note,
                         const std::string& commit) override {
    return Request::from_json(call("update_request", {{"id", id}, {"action", action}, {"actor", actor},
                                                      {"note", note}, {"commit", commit}})["request"]);
  }
  int land(const std::vector<std::string>&, const std::string& = "") override {
    // Only the hub decides what reached its branches (on push).
    return 0;
  }
  std::vector<Event> events(const std::string& agent, long long after, int limit) override {
    return events_wait(agent, after, limit, 0);
  }
  // The hub holds the request open until an event arrives or wait runs out.
  std::vector<Event> events_wait(const std::string& agent, long long after, int limit, int wait) override {
    std::vector<Event> out;
    json res = call("events", {{"agent", agent}, {"after", after}, {"limit", limit}, {"wait", wait}});
    for (auto& e : res["events"])
      out.push_back(Event::from_json(e));
    return out;
  }
  long long last_event() override { return call("last_event", {}).value("last", 0LL); }
  Subscription subscribe(const std::string& agent, const std::string& url) override {
    return Subscription::from_json(call("subscribe", {{"agent", agent}, {"url", url}})["subscription"]);
  }
  bool unsubscribe(const std::string& agent, const std::string& url) override {
    return call("unsubscribe", {{"agent", agent}, {"url", url}}).value("removed", false);
  }
  std::vector<Subscription> subscriptions() override {
    std::vector<Subscription> out;
    json res = call("subscriptions", {});
    for (auto& s : res["subscriptions"]) out.push_back(Subscription::from_json(s));
    return out;
  }
  void set_subscription_cursor(long long, long long) override {
    throw std::runtime_error("subscription cursors are kept by the hub");
  }

 private:
  json call(const std::string& method, json args) {
    args["method"] = method;
    return t_.call("coord", args);
  }
  HubTransport& t_;
  std::unique_ptr<HubTransport> owned_;
};

}  // namespace

std::unique_ptr<Coordination> sql_coordination(Store& s, int lease_minutes) {
  return std::make_unique<SqlCoordination>(s, lease_minutes);
}

std::unique_ptr<Coordination> hub_coordination(HubTransport& t, std::unique_ptr<HubTransport> owned) {
  return std::make_unique<HubCoordination>(t, std::move(owned));
}

json coord_dispatch(Coordination& c, const json& q) {
  std::string m = q.value("method", "");
  if (m == "locks") {
    json out = json::array();
    for (auto& l : c.locks()) out.push_back(l.to_json());
    return {{"locks", out}};
  }
  if (m == "acquire") {
    auto b = c.acquire(LockRec::from_json(q.at("lock")));
    return {{"blocked", b ? b->to_json() : json(nullptr)}};
  }
  if (m == "release") return {{"released", c.release(q.at("target"), q.value("holder", ""), q.value("force", false))}};
  if (m == "sync_locks") {
    std::vector<LockRec> keep;
    for (auto& l : q.value("keep", json::array())) keep.push_back(LockRec::from_json(l));
    c.sync_locks(q.at("holder"), keep);
    return {{"ok", true}};
  }
  if (m == "put_work") return {{"work", c.put_work(WorkItem::from_json(q.at("work"))).to_json()}};
  if (m == "work") {
    auto w = c.work(q.at("id"));
    return {{"work", w ? w->to_json() : json(nullptr)}};
  }
  if (m == "works") {
    json out = json::array();
    for (auto& w : c.works(q.value("include_closed", false))) out.push_back(w.to_json());
    return {{"works", out}};
  }
  if (m == "add_request") return {{"request", c.add_request(Request::from_json(q.at("request"))).to_json()}};
  if (m == "request") {
    auto r = c.request(q.at("id"));
    return {{"request", r ? r->to_json() : json(nullptr)}};
  }
  if (m == "requests") {
    json out = json::array();
    for (auto& r : c.requests(RequestFilter::from_json(q.value("filter", json::object())))) out.push_back(r.to_json());
    return {{"requests", out}};
  }
  if (m == "update_request")
    return {{"request", c.update_request(q.at("id"), q.at("action"), q.value("actor", ""), q.value("note", ""),
                                         q.value("commit", ""))
                            .to_json()}};
  if (m == "events") {
    json out = json::array();
    for (auto& e : c.events(q.at("agent"), q.value("after", 0LL), q.value("limit", 100))) out.push_back(e.to_json());
    return {{"events", out}};
  }
  if (m == "last_event") return {{"last", c.last_event()}};
  if (m == "subscribe") return {{"subscription", c.subscribe(q.at("agent"), q.at("url")).to_json()}};
  if (m == "unsubscribe") return {{"removed", c.unsubscribe(q.at("agent"), q.at("url"))}};
  if (m == "subscriptions") {
    json out = json::array();
    for (auto& s : c.subscriptions()) out.push_back(s.to_json());
    return {{"subscriptions", out}};
  }
  throw std::runtime_error("unknown coordination call " + m);
}

}  // namespace hs
