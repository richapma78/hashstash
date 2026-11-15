#include "ops/locks.h"

#include <stdexcept>

#include "ops/coord.h"
#include "ops/work.h"

namespace hs {

static RemoteLockHook g_hook = nullptr;
void set_remote_lock_hook(RemoteLockHook hook) { g_hook = hook; }

static LockInfo read_lock(Stmt& st) {
  return LockInfo{st.text(0), st.text(1), st.text(2), st.text(5), st.integer(3), st.integer(4)};
}
static LockInfo from_rec(const LockRec& l) { return LockInfo{l.target, l.name, l.holder, l.work, l.acquired, l.expires}; }

std::optional<LockInfo> lock_on(Repo& repo, const std::string& target) {
  if (auto* c = repo.coordination_override()) {
    for (auto& l : c->locks())
      if (l.target == target) return from_rec(l);
    return std::nullopt;
  }
  Stmt st(repo.store().db(),
          "SELECT target, name, holder, acquired, expires, work FROM lock WHERE target = ? AND expires > ?");
  st.bind(1, target).bind(2, now_seconds());
  if (!st.step()) return std::nullopt;
  return read_lock(st);
}

std::optional<LockInfo> blocking_lock(Repo& repo, const std::string& target) {
  auto l = lock_on(repo, target);
  if (l && l->holder != repo.agent()) return l;
  return std::nullopt;
}

static std::runtime_error locked_error(const std::string& name, const LockInfo& b) {
  std::string what = b.work.empty() ? "" : " for " + b.work;
  return std::runtime_error(name + " is locked by " + b.holder + what + " until " + format_time(b.expires) +
                            ". Don't wait: log the change you need (hs_request, or hs request " + name +
                            " --need \"...\"); the holder makes it before committing. Or queue for the lock "
                            "(kind lock / --lock).");
}

LockInfo acquire_lock(Repo& repo, const std::string& target, const std::string& name) {
  std::string work = current_work_id(repo);
  long long now = now_seconds();
  LockInfo info{target, name, repo.agent(), work, now, now + 60LL * repo.config().lock_minutes};
  if (auto* c = repo.coordination_override()) {
    if (auto b = c->acquire({target, name, info.holder, work, now, info.expires})) throw locked_error(name, from_rec(*b));
    return info;
  }
  repo.store().transaction([&] {
    if (auto b = blocking_lock(repo, target)) throw locked_error(name, *b);
    if (g_hook) g_hook(repo, info, true, false);  // the hub must agree first
    Stmt st(repo.store().db(),
            "INSERT INTO lock(target, name, holder, acquired, expires, work, origin) VALUES (?, ?, ?, ?, ?, ?, '') "
            "ON CONFLICT(target) DO UPDATE SET name = excluded.name, holder = excluded.holder, "
            "acquired = excluded.acquired, expires = excluded.expires, work = excluded.work, origin = ''");
    st.bind(1, target).bind(2, name).bind(3, info.holder).bind(4, now).bind(5, info.expires).bind(6, work).run();
  });
  return info;
}

bool release_lock(Repo& repo, const std::string& target, bool force) {
  auto l = lock_on(repo, target);
  if (!l) return false;
  if (!force && l->holder != repo.agent())
    throw std::runtime_error(l->name + " is locked by " + l->holder + " (use --force to break it)");
  if (auto* c = repo.coordination_override()) return c->release(target, l->holder, true);
  if (g_hook) {
    try {
      g_hook(repo, *l, false, force);
    } catch (std::exception&) {
      // Hub unreachable: release locally; the hub lease expires on its own.
    }
  }
  // Releasing through this store's coordination also hands the lock to the
  // first request queued here (a project without a hub).
  sql_coordination(repo.store(), repo.config().lock_minutes)->release(target, l->holder, true);
  return true;
}

std::vector<LockInfo> list_locks(Repo& repo) {
  std::vector<LockInfo> out;
  if (auto* c = repo.coordination_override()) {
    for (auto& l : c->locks()) out.push_back(from_rec(l));
    return out;
  }
  Stmt st(repo.store().db(),
          "SELECT target, name, holder, acquired, expires, work FROM lock WHERE expires > ? ORDER BY name");
  st.bind(1, now_seconds());
  while (st.step()) out.push_back(read_lock(st));
  return out;
}

void renew_locks(Repo& repo) {
  long long now = now_seconds();
  long long until = now + 60LL * repo.config().lock_minutes;
  if (auto* c = repo.coordination_override()) {
    for (auto& l : c->locks())
      if (l.holder == repo.agent()) {
        LockRec r = l;
        r.expires = until;
        c->acquire(r);
      }
    return;
  }
  Stmt st(repo.store().db(), "UPDATE lock SET expires = ? WHERE holder = ? AND expires > ?");
  st.bind(1, until).bind(2, repo.agent()).bind(3, now).run();
}

int release_all(Repo& repo) {
  int n = 0;
  for (auto& l : list_locks(repo))
    if (l.holder == repo.agent()) n += release_lock(repo, l.target);
  return n;
}

}  // namespace hs
