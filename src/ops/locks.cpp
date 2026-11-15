#include "ops/locks.h"

#include <stdexcept>

namespace hs {

static LockInfo read_lock(Stmt& st) {
  return LockInfo{st.text(0), st.text(1), st.text(2), st.integer(3), st.integer(4)};
}

std::optional<LockInfo> lock_on(Repo& repo, const std::string& target) {
  Stmt st(repo.store().db(),
          "SELECT target, name, holder, acquired, expires FROM lock WHERE target = ? AND expires > ?");
  st.bind(1, target).bind(2, now_seconds());
  if (!st.step()) return std::nullopt;
  return read_lock(st);
}

std::optional<LockInfo> blocking_lock(Repo& repo, const std::string& target) {
  auto l = lock_on(repo, target);
  if (l && l->holder != repo.agent()) return l;
  return std::nullopt;
}

LockInfo acquire_lock(Repo& repo, const std::string& target, const std::string& name) {
  LockInfo info;
  repo.store().transaction([&] {
    if (auto b = blocking_lock(repo, target))
      throw std::runtime_error(name + " is locked by " + b->holder + " until " + format_time(b->expires));
    long long now = now_seconds();
    info = LockInfo{target, name, repo.agent(), now, now + 60LL * repo.config().lock_minutes};
    Stmt st(repo.store().db(),
            "INSERT INTO lock(target, name, holder, acquired, expires) VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(target) DO UPDATE SET name = excluded.name, holder = excluded.holder, "
            "acquired = excluded.acquired, expires = excluded.expires");
    st.bind(1, target).bind(2, name).bind(3, info.holder).bind(4, now).bind(5, info.expires).run();
  });
  return info;
}

bool release_lock(Repo& repo, const std::string& target, bool force) {
  auto l = lock_on(repo, target);
  if (!l) return false;
  if (!force && l->holder != repo.agent())
    throw std::runtime_error(l->name + " is locked by " + l->holder + " (use --force to break it)");
  Stmt st(repo.store().db(), "DELETE FROM lock WHERE target = ?");
  st.bind(1, target).run();
  return true;
}

std::vector<LockInfo> list_locks(Repo& repo) {
  Stmt st(repo.store().db(),
          "SELECT target, name, holder, acquired, expires FROM lock WHERE expires > ? ORDER BY name");
  st.bind(1, now_seconds());
  std::vector<LockInfo> out;
  while (st.step()) out.push_back(read_lock(st));
  return out;
}

void renew_locks(Repo& repo) {
  Stmt st(repo.store().db(), "UPDATE lock SET expires = ? WHERE holder = ? AND expires > ?");
  long long now = now_seconds();
  st.bind(1, now + 60LL * repo.config().lock_minutes).bind(2, repo.agent()).bind(3, now).run();
}

int release_all(Repo& repo) {
  int n = 0;
  for (auto& l : list_locks(repo))
    if (l.holder == repo.agent()) n += release_lock(repo, l.target);
  return n;
}

}  // namespace hs
