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

std::string TestSummary::label() const {
  if (total == 0) return needs_tests ? "no tests" : "no tests needed";
  std::string out = std::to_string(passed) + "/" + std::to_string(total) + " passed";
  if (failed) out += ", " + std::to_string(failed) + " failed";
  if (not_run) out += ", " + std::to_string(not_run) + " not run";
  return out;
}

nlohmann::json TestSummary::to_json() const {
  return {{"summary", label()}, {"total", total}, {"passed", passed}, {"failed", failed}, {"not_run", not_run}};
}

void record_test_results(Store& s, const nlohmann::json& report) {
  if (!report.is_object() || !report.contains("tests_run")) return;
  s.transaction([&] {
    Stmt rec(s.db(),
             "INSERT OR REPLACE INTO test_result(suite, test, test_hash, status, time, output) VALUES (?, ?, ?, ?, ?, ?)");
    for (auto& t : report["tests_run"]) {
      if (!t.contains("suites")) continue;
      for (auto& suite : t["suites"]) {
        rec.bind(1, suite.get<std::string>()).bind(2, t.value("test", "")).bind(3, t.value("test_hash", ""))
            .bind(4, t.value("status", "")).bind(5, now_seconds()).bind(6, t.value("output", ""));
        rec.run();
        rec.reset();
      }
    }
  });
}

std::map<std::string, TestSummary> test_summaries(Repo& repo, const Namespace& ns, const std::set<std::string>* only) {
  Store& s = repo.store();
  auto flat = flatten(s, ns);
  std::map<std::string, const FlatDef*> by_lineage;
  for (auto& f : flat) by_lineage[f.lineage] = &f;
  Stmt st(s.db(), "SELECT status FROM test_result WHERE suite = ? AND test = ?");
  // Results of verifying exactly this namespace come first: a test's outcome
  // also depends on the code it calls, so a result recorded for the same
  // function and tests in a different state (say, with a broken helper) may
  // not hold here. The verification report travels with pushes, so the hub
  // has it too.
  std::map<std::string, std::string> verified;
  {
    Stmt v(s.db(), "SELECT report FROM verification WHERE ns = ?");
    v.bind(1, put_namespace(s, ns));
    if (v.step()) try {
        auto rep = nlohmann::json::parse(v.text(0));
        for (auto& t : rep.value("tests_run", nlohmann::json::array()))
          verified[t.value("test", "")] = t.value("status", "");
      } catch (...) {
      }
  }
  std::map<std::string, TestSummary> out;
  for (auto& f : flat) {
    if (only && !only->count(f.qname)) continue;
    if (is_test_def(f.file, f.qname)) continue;
    TestSummary t;
    t.qname = f.qname;
    t.needs_tests = needs_tests(repo.config(), f.file, f.qname, f.def.kind);
    std::vector<const FlatDef*> tests;
    if (auto it = ns.tests.find(f.lineage); it != ns.tests.end())
      for (auto& tid : it->second)
        if (auto j = by_lineage.find(tid); j != by_lineage.end()) tests.push_back(j->second);
    // Same suite hash as suite_hash(): the code plus all its tests.
    std::vector<std::string> hashes;
    for (auto* x : tests) hashes.push_back(x->hash);
    std::sort(hashes.begin(), hashes.end());
    std::string body = f.hash;
    for (auto& h : hashes) body += "\n" + h;
    Hash suite = hash_bytes("suite", body);
    for (auto* x : tests) {
      std::string status = "not run";
      if (auto vit = verified.find(x->qname); vit != verified.end()) {
        status = vit->second == "passed" ? "passed" : "failed";
      } else {
        st.bind(1, suite).bind(2, x->qname);
        if (st.step()) status = st.text(0) == "passed" ? "passed" : "failed";
        st.reset();
      }
      if (status == "passed") ++t.passed;
      else if (status == "failed") ++t.failed;
      else ++t.not_run;
      t.results.push_back({x->qname, status});
    }
    t.total = (int)tests.size();
    out[f.qname] = t;
  }
  return out;
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
  std::set<std::string> one{r.qname};
  auto sums = test_summaries(repo, ns, &one);
  if (sums.count(r.qname)) j["results"] = sums[r.qname].to_json();
  if (tests.empty()) {
    auto s = suggest_tests(repo, r.qname);
    if (!s.empty()) j["suggested"] = s;
  }
  return j;
}

}  // namespace hs
