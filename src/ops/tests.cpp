#include "ops/tests.h"

#include <algorithm>
#include <set>

#include "ops/query.h"

namespace hs {

static std::string basename(const std::string& file) { return file.substr(file.rfind('/') + 1); }

static std::string short_of(const std::string& qname) {
  std::string path = qname.substr(qname.find(':') + 1);
  std::string last = path.substr(path.rfind('.') + 1);
  return last.substr(0, last.find('~'));
}

bool is_test_file(const std::string& file) {
  std::string b = basename(file);
  if (starts_with(b, "test_") || b == "conftest.py") return true;
  size_t dot = b.rfind('.');
  if (dot != std::string::npos && ends_with(b.substr(0, dot), "_test")) return true;
  std::string f = "/" + file;
  return f.find("/tests/") != std::string::npos || f.find("/test/") != std::string::npos;
}

bool is_test_def(const std::string& file, const std::string& qname) {
  return is_test_file(file) && starts_with(short_of(qname), "test");
}

bool needs_tests(const Config& c, const std::string& file, const std::string& qname,
                 const std::string& kind) {
  if (c.tests_require == "off") return false;
  if (kind != "function" && kind != "method") return false;
  if (is_test_file(file)) return false;
  std::string s = short_of(qname);
  for (auto& p : c.tests_exempt)
    if (glob_match(p, s)) return false;
  return true;
}

std::vector<TestWarning> missing_tests(Repo& repo, const Namespace& ns,
                                       const std::vector<std::string>* only) {
  std::vector<TestWarning> out;
  auto check = [&](const IndexRow& r) {
    if (!needs_tests(repo.config(), r.file, r.qname, r.kind)) return;
    auto it = ns.tests.find(r.lineage);
    if (it != ns.tests.end() && !it->second.empty()) return;
    out.push_back({r.qname, "no test cases linked (hs tests link " + r.qname + " <test>...)"});
  };
  if (only) {
    for (auto& q : *only)
      if (auto r = row_by_qname(repo, q)) check(*r);
  } else {
    for (auto& r : list_defs(repo, "")) check(r);
  }
  return out;
}

static std::string lineage_of(Repo& repo, const std::string& name) {
  IndexRow r = resolve_name(repo, name);
  if (r.lineage.empty()) throw std::runtime_error("no lineage for " + r.qname);
  return r.lineage;
}

void link_tests(Repo& repo, const std::string& def, const std::vector<std::string>& tests) {
  Namespace ns = repo.work_ns();
  std::string id = lineage_of(repo, def);
  for (auto& t : tests) {
    std::string tid = lineage_of(repo, t);
    if (tid == id) throw std::runtime_error("a definition can't be its own test");
    ns.tests[id].insert(tid);
  }
  repo.set_work_ns(ns, "tests", "link tests to " + def);
}

void unlink_tests(Repo& repo, const std::string& def, const std::vector<std::string>& tests) {
  Namespace ns = repo.work_ns();
  std::string id = lineage_of(repo, def);
  auto it = ns.tests.find(id);
  if (it == ns.tests.end()) return;
  for (auto& t : tests) it->second.erase(lineage_of(repo, t));
  if (it->second.empty()) ns.tests.erase(it);
  repo.set_work_ns(ns, "tests", "unlink tests from " + def);
}

std::vector<std::string> suggest_tests(Repo& repo, const std::string& def) {
  std::vector<std::string> out;
  Stmt st(repo.store().db(),
          "SELECT DISTINCT e.caller, n.file FROM edge e JOIN name_index n ON n.qname = e.caller "
          "WHERE e.callee = ? AND e.confidence IN ('exact','likely') ORDER BY e.caller");
  st.bind(1, def);
  while (st.step())
    if (is_test_def(st.text(1), st.text(0))) out.push_back(st.text(0));
  return out;
}

int autolink_tests(Repo& repo) {
  Namespace ns = repo.work_ns();
  int made = 0;
  for (auto& r : list_defs(repo, "")) {
    if (!needs_tests(repo.config(), r.file, r.qname, r.kind)) continue;
    if (ns.tests.count(r.lineage) && !ns.tests[r.lineage].empty()) continue;
    for (auto& t : suggest_tests(repo, r.qname)) {
      auto tr = row_by_qname(repo, t);
      if (!tr || tr->lineage.empty()) continue;
      ns.tests[r.lineage].insert(tr->lineage);
      ++made;
    }
  }
  if (made) repo.set_work_ns(ns, "tests", "autolink " + std::to_string(made) + " tests");
  return made;
}

static std::vector<IndexRow> linked_rows(Repo& repo, const Namespace& ns, const IndexRow& r) {
  std::vector<IndexRow> out;
  auto it = ns.tests.find(r.lineage);
  if (it == ns.tests.end()) return out;
  for (auto& tid : it->second)
    for (auto& tr : rows_by_lineage(repo, tid)) out.push_back(tr);
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.qname < b.qname; });
  return out;
}

Hash suite_hash(Repo& repo, const Namespace& ns, const std::string& qname) {
  auto r = row_by_qname(repo, qname);
  if (!r) return "";
  std::vector<std::string> hashes;
  for (auto& t : linked_rows(repo, ns, *r)) hashes.push_back(t.hash);
  std::sort(hashes.begin(), hashes.end());
  std::string body = r->hash;
  for (auto& h : hashes) body += "\n" + h;
  return hash_bytes("suite", body);
}

nlohmann::json tests_info(Repo& repo, const std::string& qname) {
  Namespace ns = repo.work_ns();
  IndexRow r = resolve_name(repo, qname);
  nlohmann::json tests = nlohmann::json::array();
  for (auto& t : linked_rows(repo, ns, r))
    tests.push_back({{"name", t.qname}, {"hash", short_hash(t.hash)}});
  nlohmann::json j = {{"name", r.qname},
                      {"hash", short_hash(r.hash)},
                      {"suite_hash", short_hash(suite_hash(repo, ns, r.qname))},
                      {"tests", tests},
                      {"needs_tests", needs_tests(repo.config(), r.file, r.qname, r.kind)}};
  if (tests.empty()) {
    auto s = suggest_tests(repo, r.qname);
    if (!s.empty()) j["suggested"] = s;
  }
  return j;
}

}  // namespace hs
