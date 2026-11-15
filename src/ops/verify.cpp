#include "ops/verify.h"

#include <cstdio>
#include <map>
#include <set>

#include "ops/diff.h"
#include "ops/import_render.h"
#include "ops/query.h"
#include "ops/tests.h"

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#else
#include <sys/wait.h>
#endif

namespace hs {

namespace {

struct RunResult {
  int code = -1;
  std::string output;
};

RunResult run_in(const fs::path& dir, const std::string& command) {
#ifdef _WIN32
  std::string cmd = "cd /d \"" + dir.string() + "\" && " + command + " 2>&1";
#else
  std::string cmd = "cd '" + dir.string() + "' && " + command + " 2>&1";
#endif
  RunResult r;
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return r;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, p)) > 0) r.output.append(buf, n);
  int status = pclose(p);
#ifdef _WIN32
  r.code = status;
#else
  r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  if (r.output.size() > 8000) r.output = "..." + r.output.substr(r.output.size() - 8000);
  return r;
}

// "tests/test_a.py:TestX.test_y~2" -> "tests/test_a.py::TestX::test_y"
std::string pytest_node(const std::string& qname) {
  size_t colon = qname.find(':');
  std::string file = qname.substr(0, colon), path = qname.substr(colon + 1), out = file;
  size_t i = 0;
  while (i <= path.size()) {
    size_t d = path.find('.', i);
    if (d == std::string::npos) d = path.size();
    std::string comp = path.substr(i, d - i);
    out += "::" + comp.substr(0, comp.find('~'));
    i = d + 1;
  }
  return out;
}

std::string command_for(const Repo& repo, const std::string& lang, const std::vector<std::string>& nodes) {
  std::string tmpl = lang == "python" ? repo.config().test_command_python : "";
  if (tmpl.empty()) return "";
  std::string list;
  for (auto& n : nodes) list += (list.empty() ? "" : " ") + std::string("\"") + n + "\"";
  size_t at = tmpl.find("{tests}");
  if (at == std::string::npos) return tmpl + " " + list;
  return tmpl.replace(at, 7, list);
}

}  // namespace

std::string verification_status(Repo& repo, const Hash& ns) {
  Stmt st(repo.store().db(), "SELECT status FROM verification WHERE ns = ?");
  st.bind(1, ns);
  return st.step() ? st.text(0) : "";
}

nlohmann::json verify(Repo& repo, bool all) {
  Store& s = repo.store();
  Hash work_hash = repo.work_ns_hash();
  Namespace work = load_namespace(s, work_hash);
  nlohmann::json report = {{"namespace", short_hash(work_hash)}};
  nlohmann::json failures = nlohmann::json::array(), warnings = nlohmann::json::array();

  // Which definitions are under review.
  std::vector<IndexRow> targets;
  if (all) {
    targets = list_defs(repo, "");
  } else {
    auto d = diff_namespaces(s, load_namespace(s, repo.head_ns_hash()), work);
    for (auto& c : d.defs)
      if (c.kind != "removed")
        if (auto r = row_by_qname(repo, c.new_name)) targets.push_back(*r);
  }
  nlohmann::json tnames = nlohmann::json::array();
  for (auto& t : targets) tnames.push_back(t.qname);
  report["definitions"] = tnames;

  // Policy: linked tests and size.
  std::vector<std::string> names;
  for (auto& t : targets) names.push_back(t.qname);
  for (auto& w : missing_tests(repo, work, &names)) {
    nlohmann::json j = {{"name", w.qname}, {"problem", "no test cases linked"}};
    (repo.config().tests_require == "block" ? failures : warnings).push_back(j);
  }
  for (auto& t : targets) {
    Stmt st(s.db(), "SELECT lines FROM def_info WHERE hash = ?");
    st.bind(1, t.hash);
    if (!st.step()) continue;
    int lines = (int)st.integer(0);
    if (repo.config().max_lines > 0 && lines > repo.config().max_lines)
      failures.push_back({{"name", t.qname}, {"problem", "too long: " + std::to_string(lines) + " lines"}});
    else if (repo.config().warn_lines > 0 && lines > repo.config().warn_lines)
      warnings.push_back({{"name", t.qname}, {"problem", "long: " + std::to_string(lines) + " lines; consider splitting"}});
  }

  // Tests to run: tests linked to the targets, plus targets that are tests.
  std::map<std::string, std::set<std::string>> suites_of_test;  // test -> suite hashes
  std::map<std::string, IndexRow> test_rows;
  for (auto& t : targets) {
    if (is_test_def(t.file, t.qname)) {
      test_rows[t.qname] = t;
      continue;
    }
    auto it = work.tests.find(t.lineage);
    if (it == work.tests.end()) continue;
    Hash suite = suite_hash(repo, work, t.qname);
    for (auto& tid : it->second)
      for (auto& tr : rows_by_lineage(repo, tid)) {
        test_rows[tr.qname] = tr;
        suites_of_test[tr.qname].insert(suite);
      }
  }

  nlohmann::json ran = nlohmann::json::array();
  if (!test_rows.empty()) {
    fs::path tree = repo.hs_dir() / "verify" / "tree";
    render_to(repo, tree, work_hash);
    // Batch per file; if a batch fails, run its tests one by one to attribute.
    std::map<std::string, std::vector<const IndexRow*>> by_file;
    for (auto& [q, r] : test_rows) by_file[r.file].push_back(&r);
    for (auto& [file, rows] : by_file) {
      std::vector<std::string> nodes;
      for (auto* r : rows) nodes.push_back(pytest_node(r->qname));
      std::string cmd = command_for(repo, rows[0]->lang, nodes);
      if (cmd.empty()) {
        warnings.push_back({{"name", file}, {"problem", "no test command for " + rows[0]->lang}});
        continue;
      }
      RunResult batch = run_in(tree, cmd);
      for (auto* r : rows) {
        RunResult one = batch;
        if (batch.code != 0 && rows.size() > 1) one = run_in(tree, command_for(repo, r->lang, {pytest_node(r->qname)}));
        std::string status = one.code == 0 ? "passed" : "failed";
        nlohmann::json j = {{"test", r->qname}, {"status", status}};
        if (one.code != 0) {
          j["output"] = one.output;
          failures.push_back({{"name", r->qname}, {"problem", "test failed"}});
        }
        ran.push_back(j);
        Stmt rec(s.db(),
                 "INSERT OR REPLACE INTO test_result(suite, test, test_hash, status, time, output) "
                 "VALUES (?, ?, ?, ?, ?, ?)");
        std::set<std::string> suites = suites_of_test[r->qname];
        if (suites.empty()) suites.insert("");
        for (auto& suite : suites) {
          rec.bind(1, suite).bind(2, r->qname).bind(3, r->hash).bind(4, status)
              .bind(5, now_seconds()).bind(6, one.code == 0 ? "" : one.output).run();
          rec.reset();
        }
      }
    }
  }

  std::string status = failures.empty() ? "passed" : "failed";
  report["status"] = status;
  report["tests_run"] = ran;
  report["failures"] = failures;
  report["warnings"] = warnings;
  Stmt st(s.db(),
          "INSERT OR REPLACE INTO verification(ns, status, time, agent, report) VALUES (?, ?, ?, ?, ?)");
  st.bind(1, work_hash).bind(2, status).bind(3, now_seconds()).bind(4, repo.agent()).bind(5, report.dump()).run();
  repo.record("verify", work_hash, "", status + ": " + std::to_string(ran.size()) + " tests, " +
                                           std::to_string(failures.size()) + " failures");
  return report;
}

}  // namespace hs
