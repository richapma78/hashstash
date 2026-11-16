#include "ops/verify.h"

#include "ops/work.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

#include "ops/diff.h"
#include "ops/import_render.h"
#include "ops/query.h"
#include "ops/tests.h"

namespace hs {

namespace {

RunResult run_in(const fs::path& dir, const std::string& command) {
  RunResult r = run_command(dir, command);
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

// The directory (relative to the rendered tree, "" for its top) that a Python
// test file should be run from: the nearest folder with a pytest config, else
// the folder above the package holding the test, stepping out of tests/.
// "tests/fixtures/app/tests/test_x.py" -> "tests/fixtures/app".
std::string python_root(const fs::path& tree, const std::string& file) {
  std::vector<std::string> parts;
  for (size_t i = 0, j; i <= file.size(); i = j + 1) {
    j = file.find('/', i);
    if (j == std::string::npos) j = file.size();
    parts.push_back(file.substr(i, j - i));
  }
  parts.pop_back();  // the file name
  auto rel = [&](size_t n) {
    std::string r;
    for (size_t k = 0; k < n; ++k) r += (k ? "/" : "") + parts[k];
    return r;
  };
  auto dir = [&](size_t n) { return n ? tree / from_utf8(rel(n)) : tree; };
  for (size_t n = parts.size() + 1; n-- > 0;)
    for (const char* cfg : {"pytest.ini", "pyproject.toml", "setup.cfg", "tox.ini"})
      if (fs::exists(dir(n) / cfg)) return rel(n);
  size_t n = parts.size();
  while (n > 0 && fs::exists(dir(n) / "__init__.py")) --n;
  if (n > 0 && (parts[n - 1] == "tests" || parts[n - 1] == "test" || parts[n - 1] == "testing")) --n;
  return rel(n);
}

// The res:// path of a file inside the Godot project that contains it.
std::string godot_res_path(const fs::path& tree, const std::string& file) {
  fs::path dir = from_utf8(file).parent_path();
  while (true) {
    if (fs::exists(tree / dir / "project.godot")) {
      std::string rel = to_generic(fs::relative(tree / from_utf8(file), tree / dir));
      return "res://" + rel;
    }
    if (dir.empty() || dir == dir.parent_path()) break;
    dir = dir.parent_path();
  }
  return "res://" + file;
}

// The name a C/C++ test framework filters by, from the test's signature:
//   TEST(Suite, Name), TEST_F(...)        -> Suite.Name     (GoogleTest)
//   TEST_P(Suite, Name)                   -> */Suite.Name/*
//   TYPED_TEST(Suite, Name)               -> Suite/*.Name
//   TYPED_TEST_P(Suite, Name)             -> */Suite/*.Name
//   TEST_CASE("adds up", "[tag]")         -> adds up        (Catch2, doctest)
//   SCENARIO("adds up")                   -> *Scenario: adds up
//   BOOST_AUTO_TEST_CASE(name), TEST(name) -> name
//   void test_add()                       -> test_add       (a plain function)
std::string cpp_test_id(const std::string& sig, const std::string& qname) {
  size_t paren = sig.find('(');
  std::string macro = paren == std::string::npos ? "" : trim(sig.substr(0, paren));
  bool is_macro = !macro.empty() && sig.back() == ')';
  for (char c : macro)
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) is_macro = false;
  if (!is_macro) {
    std::string path = qname.substr(qname.find(':') + 1);
    std::string last = path.substr(path.rfind('.') + 1);
    last = last.substr(0, last.find('~'));
    return last.substr(last.rfind("::") == std::string::npos ? 0 : last.rfind("::") + 2);
  }
  std::string body = sig.substr(paren + 1, sig.size() - paren - 2);
  // A string argument names the test (Catch2, doctest; later strings are tags).
  if (size_t q = body.find('"'); q != std::string::npos) {
    std::string name;
    for (size_t i = q + 1; i < body.size() && body[i] != '"'; ++i) {
      if (body[i] == '\\' && i + 1 < body.size()) ++i;
      name += body[i];
    }
    return starts_with(macro, "SCENARIO") ? "*Scenario: " + name : name;
  }
  // Otherwise identifiers: top-level arguments.
  std::vector<std::string> args;
  std::string cur;
  int depth = 0;
  for (char c : body) {
    if (c == '(' || c == '<' || c == '[') ++depth;
    if (c == ')' || c == '>' || c == ']') --depth;
    if (c == ',' && depth == 0) {
      args.push_back(trim(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  args.push_back(trim(cur));
  if (args.size() >= 2 && !args[0].empty() && !args[1].empty() && macro.find("BOOST") == std::string::npos) {
    if (macro == "TEST_P") return "*/" + args[0] + "." + args[1] + "/*";
    if (macro == "TYPED_TEST") return args[0] + "/*." + args[1];
    if (macro == "TYPED_TEST_P") return "*/" + args[0] + "/*." + args[1];
    return args[0] + "." + args[1];
  }
  return args.empty() ? macro : args[0];
}

std::string signature_of(Store& s, const Hash& def) {
  Stmt st(s.db(), "SELECT signature FROM def_info WHERE hash = ?");
  st.bind(1, def);
  return st.step() ? st.text(0) : "";
}

std::string command_for(const Repo& repo, const std::string& lang, const std::vector<std::string>& nodes) {
  if (lang == "cpp") {
    // nodes: test names as the framework filters them; one quoted argument.
    std::string tmpl = repo.config().test_command_cpp;
    if (tmpl.empty()) return "";
    std::string join = repo.config().test_join_cpp;
    if (join.empty()) join = tmpl.find("gtest") != std::string::npos ? ":" : ",";
    std::string list;
    for (auto& n : nodes) {
      std::string e;
      for (char c : n) {
        if (c == '"' || (join == "," && c == ',')) e += '\\';  // Catch2 / doctest: "a\,b" is one name
        e += c;
      }
      list += (list.empty() ? "" : join) + e;
    }
    list = "\"" + list + "\"";
    size_t at = tmpl.find("{tests}");
    if (at == std::string::npos) return tmpl + " " + list;
    return tmpl.replace(at, 7, list);
  }
  if (lang == "gdscript") {
    // nodes: "<res path>::<test>" (or just "<res path>" for the whole file)
    std::string tmpl = repo.config().test_command_gdscript;
    if (tmpl.empty() || nodes.empty()) return "";
    std::string file = nodes[0].substr(0, nodes[0].find("::"));
    std::string test = nodes.size() == 1 && nodes[0].find("::") != std::string::npos
                           ? nodes[0].substr(nodes[0].find("::") + 2) : "";
    for (auto [key, val] : {std::pair<std::string, std::string>{"{file}", file}, {"{tests}", test}})
      for (size_t at; (at = tmpl.find(key)) != std::string::npos;) tmpl.replace(at, key.size(), val);
    return tmpl;
  }
  std::string tmpl = lang == "python" ? repo.config().test_command_python : "";
  if (tmpl.empty()) return "";
  std::string list;
  for (auto& n : nodes) list += (list.empty() ? "" : " ") + std::string("\"") + n + "\"";
  size_t at = tmpl.find("{tests}");
  if (at == std::string::npos) return tmpl + " " + list;
  return tmpl.replace(at, 7, list);
}

// What verify reviews against: the last state of this branch known to a hub
// (pushed or pulled), so everything a push would send gets checked, whether or
// not it is committed yet. Before the first push that is the empty namespace.
Hash verify_base(Repo& repo) {
  Store& s = repo.store();
  std::string suffix = "/" + repo.branch();
  for (auto& [name, value] : s.refs_with_prefix("remote/")) {
    if (value.empty() || name.size() <= suffix.size() + 7) continue;
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
    if (!s.has(value)) continue;
    return load_commit(s, value).ns;
  }
  return empty_namespace(s);
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
    auto d = diff_namespaces(s, load_namespace(s, verify_base(repo)), work);
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
    if (t.kind == "namespace") continue;
    Stmt st(s.db(), "SELECT lines FROM def_info WHERE hash = ?");
    st.bind(1, t.hash);
    if (!st.step()) continue;
    int lines = (int)st.integer(0);
    if (repo.config().max_lines > 0 && lines > repo.config().max_lines)
      failures.push_back({{"name", t.qname}, {"problem", "too long: " + std::to_string(lines) + " lines"}});
    else if (repo.config().warn_lines > 0 && lines > repo.config().warn_lines)
      warnings.push_back({{"name", t.qname}, {"problem", "long: " + std::to_string(lines) + " lines; consider splitting"}});
  }

  // Tests to run: tests linked to the targets, targets that are tests, and the
  // same for everything that calls a target (up to impact_depth levels), since
  // a change to add() can break mul() which calls it.
  std::map<std::string, std::set<std::string>> suites_of_test;  // test -> suite hashes
  std::map<std::string, IndexRow> test_rows;
  std::set<std::string> covered;
  nlohmann::json impacted = nlohmann::json::array();
  auto add_tests_of = [&](const IndexRow& t) {
    if (!covered.insert(t.qname).second) return;
    if (is_test_def(t.file, t.qname)) {
      test_rows[t.qname] = t;
      return;
    }
    auto it = work.tests.find(t.lineage);
    if (it == work.tests.end()) return;
    Hash suite = suite_hash(repo, work, t.qname);
    for (auto& tid : it->second)
      for (auto& tr : rows_by_lineage(repo, tid)) {
        test_rows[tr.qname] = tr;
        suites_of_test[tr.qname].insert(suite);
      }
  };
  for (auto& t : targets) add_tests_of(t);
  int depth = repo.config().tests_impact_depth;
  if (depth > 0)
    for (auto& t : targets) {
      if (t.kind == "namespace" || is_test_def(t.file, t.qname)) continue;
      nlohmann::json r = reach(repo, t.qname, depth, true);
      for (auto& level : r["levels"])
        for (auto& e : level) {
          std::string caller = e["name"].get<std::string>();
          if (covered.count(caller)) continue;
          auto row = row_by_qname(repo, caller);
          if (!row) continue;
          if (!is_test_def(row->file, row->qname)) impacted.push_back(caller);
          add_tests_of(*row);
        }
    }
  report["impacted"] = impacted;

  nlohmann::json ran = nlohmann::json::array();
  if (!test_rows.empty()) {
    fs::path tree = repo.hs_dir() / "verify" / "tree";
    render_to(repo, tree, work_hash);
    // Python trusts a cached .pyc when the source's size and mtime second match,
    // so an edit like "a + b" -> "a - b" rendered within the same second would
    // run the old code. Drop the caches and stop new ones being written.
    std::vector<fs::path> caches;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(tree, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
      if (it->is_directory() && it->path().filename() == "__pycache__") {
        caches.push_back(it->path());
        it.disable_recursion_pending();
      }
    for (auto& c : caches) fs::remove_all(c, ec);
#ifdef _WIN32
    _putenv_s("PYTHONDONTWRITEBYTECODE", "1");
#else
    setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
#endif
    // Batch per file; if a batch fails, run its tests one by one to attribute.
    bool cpp_built = false;
    RunResult cpp_build{0, ""};  // code stays 0 unless the build fails
    std::map<std::string, std::vector<const IndexRow*>> by_file;
    for (auto& [q, r] : test_rows) by_file[r.file].push_back(&r);
    for (auto& [file, rows] : by_file) {
      // Run from the test file's own project root so its imports resolve,
      // with node ids relative to that root.
      std::string root = rows[0]->lang == "python" ? python_root(tree, file) : "";
      fs::path dir = root.empty() ? tree : tree / from_utf8(root);
      bool gd = rows[0]->lang == "gdscript", cpp = rows[0]->lang == "cpp";
      auto node_of = [&](const IndexRow* r) {
        if (cpp) return cpp_test_id(signature_of(s, r->hash), r->qname);
        if (gd) return godot_res_path(tree, file) + "::" + r->qname.substr(r->qname.rfind('.') == std::string::npos
                                                                                ? r->qname.find(':') + 1
                                                                                : r->qname.rfind('.') + 1);
        std::string n = pytest_node(r->qname);
        return root.empty() ? n : n.substr(root.size() + 1);
      };
      std::vector<std::string> nodes;
      if (gd && rows.size() > 1) nodes.push_back(godot_res_path(tree, file));  // the whole file in one run
      else
        for (auto* r : rows) nodes.push_back(node_of(r));
      std::string cmd = command_for(repo, rows[0]->lang, nodes);
      if (cmd.empty()) {
        warnings.push_back({{"name", file}, {"problem", "no test command for " + rows[0]->lang +
                                                            " (set [tests] " + rows[0]->lang + " in .hs/config.toml)"}});
        continue;
      }
      // C/C++: build once, before the first C++ test runs; if the build
      // fails, every C++ test fails with its output.
      if (cpp && !cpp_built) {
        cpp_built = true;
        if (!repo.config().test_build_cpp.empty()) cpp_build = run_in(tree, repo.config().test_build_cpp);
        if (cpp_build.code != 0) {
          cpp_build.output = "C++ build failed ([tests] cpp_build):\n" + cpp_build.output;
          failures.push_back({{"name", "[tests] cpp_build"}, {"problem", "C++ build failed"}});
        }
      }
      bool build_failed = cpp && cpp_build.code != 0;
      RunResult batch = build_failed ? cpp_build : run_in(dir, cmd);
      for (auto* r : rows) {
        RunResult one = batch;
        if (batch.code != 0 && rows.size() > 1 && !build_failed)
          one = run_in(dir, command_for(repo, r->lang, {node_of(r)}));
        std::string status = one.code == 0 ? "passed" : "failed";
        nlohmann::json j = {{"test", r->qname}, {"status", status}, {"test_hash", r->hash}};
        {
          nlohmann::json su = nlohmann::json::array();
          for (auto& x : suites_of_test[r->qname])
            if (!x.empty()) su.push_back(x);
          j["suites"] = su;
        }
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

  // Per-function summary for everything under review or impacted.
  {
    std::set<std::string> fn;
    for (auto& t : targets) fn.insert(t.qname);
    for (auto& n : impacted) fn.insert(n.get<std::string>());
    nlohmann::json funcs = nlohmann::json::array();
    for (auto& [q, t] : test_summaries(repo, work, &fn)) {
      if (t.total == 0 && !t.needs_tests) continue;
      nlohmann::json j = t.to_json();
      j["name"] = q;
      funcs.push_back(j);
    }
    report["functions"] = funcs;
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
  if (status == "passed") {
    try {  // no hub: requests resolved on this branch are done now their tests passed
      if (int n = land_verified(repo)) report["requests_done"] = n;
    } catch (std::exception& e) {
      report["warnings"].push_back({{"name", "requests"}, {"problem", std::string("could not mark requests done: ") + e.what()}});
    }
  }
  return report;
}

}  // namespace hs
