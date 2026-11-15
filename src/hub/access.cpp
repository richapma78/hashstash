#include "hub/access.h"

#include <stdexcept>

#include "core/hash.h"
#include "core/util.h"

namespace hs {

using json = nlohmann::json;

namespace {

const long long kSessionSeconds = 12 * 3600;
const int kPasswordRounds = 200000;

bool same(const std::string& a, const std::string& b) {  // constant time for equal lengths
  if (a.size() != b.size()) return false;
  unsigned char d = 0;
  for (size_t i = 0; i < a.size(); ++i) d |= (unsigned char)(a[i] ^ b[i]);
  return d == 0;
}

// Salted and stretched with BLAKE3 (the hash this project already ships).
// Stored as "b3s$<rounds>$<salt>$<hash>".
std::string stretch(const std::string& salt, const std::string& password, int rounds) {
  Hash h = hash_bytes("password", salt + "\n" + password);
  for (int i = 1; i < rounds; ++i) h = hash_bytes("password", h + salt);
  return h;
}

std::string hash_password(const std::string& password) {
  std::string salt = random_hex(16);
  return "b3s$" + std::to_string(kPasswordRounds) + "$" + salt + "$" + stretch(salt, password, kPasswordRounds);
}

bool check_password(const std::string& stored, const std::string& password) {
  size_t a = stored.find('$'), b = stored.find('$', a + 1), c = stored.find('$', b + 1);
  if (a == std::string::npos || b == std::string::npos || c == std::string::npos) return false;
  int rounds = std::stoi(stored.substr(a + 1, b - a - 1));
  std::string salt = stored.substr(b + 1, c - b - 1);
  return same(stretch(salt, password, rounds), stored.substr(c + 1));
}

std::string session_id(const std::string& token) { return "web/" + hash_bytes("session", token); }

}  // namespace

json AccessKey::to_json(bool with_hash) const {
  json j = {{"id", id}, {"role", role}, {"project", project}, {"work", work}, {"scope", scope}, {"label", label},
            {"issued_by", issued_by}, {"created", created}, {"expires", expires}, {"revoked", revoked}};
  if (with_hash) j["secret_hash"] = secret_hash;
  return j;
}

AccessKey AccessKey::from_json(const json& j) {
  AccessKey k;
  k.id = j.value("id", "");
  k.secret_hash = j.value("secret_hash", "");
  k.role = j.value("role", "");
  k.project = j.value("project", "");
  k.work = j.value("work", "");
  k.scope = j.value("scope", "");
  k.label = j.value("label", "");
  k.issued_by = j.value("issued_by", "");
  k.created = j.value("created", 0LL);
  k.expires = j.value("expires", 0LL);
  k.revoked = j.value("revoked", 0LL);
  return k;
}

json Principal::to_json() const {
  return {{"role", role}, {"key", key_id}, {"project", project}, {"work", work}, {"scope", scope},
          {"label", label}, {"expires", expires}};
}

bool Access::has_users() { return b_.user_count() > 0; }

void Access::add_user(const std::string& name, const std::string& password) {
  if (trim(name).empty()) throw std::runtime_error("a user needs a name");
  if (password.size() < 10) throw std::runtime_error("use a password of at least 10 characters");
  b_.put_user({{"name", trim(name)}, {"password", hash_password(password)}, {"created", now_seconds()}});
}

std::string Access::sign_in(const std::string& name, const std::string& password) {
  json u = b_.user(trim(name));
  // Do the same work whether or not the user exists, so timing doesn't tell.
  bool ok = check_password(u.is_null() ? std::string("b3s$") + std::to_string(kPasswordRounds) + "$x$y"
                                       : u.value("password", ""),
                           password) &&
            !u.is_null();
  if (!ok) throw std::runtime_error("wrong name or password");
  std::string token = random_hex(32);
  b_.save_session(session_id(token), {{"user", u.value("name", "")}}, now_seconds() + kSessionSeconds);
  return token;
}

std::optional<std::string> Access::session_user(const std::string& token) {
  if (token.empty()) return std::nullopt;
  json s = b_.load_session(session_id(token));
  if (s.is_null() || s.value("user", "").empty()) return std::nullopt;
  return s.value("user", "");
}

void Access::sign_out(const std::string& token) {
  if (!token.empty()) b_.drop_session(session_id(token));
}

Access::Issued Access::make(AccessKey k) {
  std::string secret = random_hex(24);
  k.id = random_hex(5);
  k.secret_hash = hash_bytes("access-key", secret);
  k.created = now_seconds();
  b_.put_key(k.to_json(true));
  return {k, "hsk_" + k.id + "_" + secret};
}

Access::Issued Access::issue_orchestrator(const std::string& project, const std::string& user, int days,
                                          const std::string& label) {
  if (user.empty()) throw std::runtime_error("only a signed-in person can create an orchestrator key");
  if (days < 1 || days > 366) throw std::runtime_error("an orchestrator key lasts 1 to 366 days");
  AccessKey k;
  k.role = "orchestrator";
  k.project = project;
  k.label = label.empty() ? "orchestrator" : label;
  k.issued_by = "user:" + user;
  k.expires = now_seconds() + days * 86400LL;
  Issued out = make(k);
  set_required(project, true);
  return out;
}

Access::Issued Access::issue_worker(const Principal& by, const std::string& work, const std::string& scope, int hours,
                                    const std::string& label) {
  if (!by.is_orchestrator()) throw std::runtime_error("only an orchestrator key can issue worker keys");
  if (trim(work).empty()) throw std::runtime_error("a worker key is for one work item (e.g. FEAT-12)");
  if (hours < 1) throw std::runtime_error("a worker key lasts at least an hour");
  AccessKey k;
  k.role = "worker";
  k.project = by.project;
  k.work = trim(work);
  k.scope = scope;
  k.label = label;
  k.issued_by = "key:" + by.key_id;
  k.expires = std::min(by.expires, now_seconds() + hours * 3600LL);
  return make(k);
}

Principal Access::authenticate(const std::string& project, const std::string& text) {
  Principal p;
  if (text.empty()) return p;
  // hsk_<id>_<secret>
  if (!starts_with(text, "hsk_")) throw std::runtime_error("that is not a hashstash access key");
  size_t sep = text.find('_', 4);
  if (sep == std::string::npos) throw std::runtime_error("that is not a hashstash access key");
  json j = b_.key(text.substr(4, sep - 4));
  long long now = now_seconds();
  if (j.is_null()) throw std::runtime_error("unknown access key");
  AccessKey k = AccessKey::from_json(j);
  if (!same(hash_bytes("access-key", text.substr(sep + 1)), k.secret_hash)) throw std::runtime_error("unknown access key");
  if (k.project != project) throw std::runtime_error("this access key is for project " + k.project + ", not " + project);
  if (k.revoked) throw std::runtime_error("this access key was revoked");
  if (k.expires <= now) throw std::runtime_error("this access key expired " + format_time(k.expires));
  if (starts_with(k.issued_by, "key:")) {  // issued by an orchestrator key, which must still be valid
    json pj = b_.key(k.issued_by.substr(4));
    if (pj.is_null() || !AccessKey::from_json(pj).active(now))
      throw std::runtime_error("the orchestrator key that issued this key is no longer valid");
  }
  p.role = k.role;
  p.key_id = k.id;
  p.project = k.project;
  p.work = k.work;
  p.scope = k.scope;
  p.label = k.label;
  p.expires = k.expires;
  return p;
}

std::vector<AccessKey> Access::keys(const std::string& project) {
  std::vector<AccessKey> out;
  for (auto& j : b_.keys(project)) out.push_back(AccessKey::from_json(j));
  return out;
}

void Access::revoke(const std::string& project, const std::string& id, const std::string& by_user,
                    const Principal* by_key) {
  json j = b_.key(id);
  if (j.is_null()) throw std::runtime_error("no key " + id);
  AccessKey k = AccessKey::from_json(j);
  if (k.project != project) throw std::runtime_error("key " + id + " is not for project " + project);
  if (by_user.empty()) {
    if (!by_key || !by_key->is_orchestrator()) throw std::runtime_error("only a person or an orchestrator can revoke keys");
    if (k.role != "worker") throw std::runtime_error("orchestrator keys are revoked by a person, on the hub's pages");
  }
  if (!k.revoked) {
    k.revoked = now_seconds();
    b_.put_key(k.to_json(true));
  }
}

bool Access::required(const std::string& project) { return b_.setting("require_keys/" + project) == "1"; }
void Access::set_required(const std::string& project, bool on) { b_.set_setting("require_keys/" + project, on ? "1" : "0"); }

bool Access::in_scope(const std::string& scope, const std::string& name) {
  if (trim(scope).empty()) return true;
  std::string file = name.substr(0, name.find(':'));
  std::string rest = scope + ",", glob;
  for (char c : rest) {
    if (c != ',') {
      glob += c;
      continue;
    }
    std::string g = trim(glob);
    glob.clear();
    if (g.empty()) continue;
    if (glob_match(g, file) || glob_match(g, name)) return true;
  }
  return false;
}

}  // namespace hs
