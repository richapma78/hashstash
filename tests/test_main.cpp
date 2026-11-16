// Unit tests. Set HS_ROUNDTRIP_DIR to a folder of Python code to also check
// that every file in it imports and renders back byte-for-byte.
#include <cstdlib>
#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "core/hash.h"
#include "core/model.h"
#include "core/util.h"
#include "lang/lang.h"
#include "ops/build.h"
#include "ops/context.h"
#include "ops/edit.h"
#include "hub/backend.h"
#include "hub/access.h"
#include "hub/service.h"
#include "hub/workspace.h"
#include "mcp/server.h"
#include "ops/work.h"
#include "ops/history.h"
#include "ops/locks.h"
#include "ops/publish.h"
#include "ops/sync.h"
#include "ops/query.h"
#include "ops/tests.h"
#include "ops/verify.h"
#include "ops/import_render.h"
#include "ops/repo.h"
#include "ops/review.h"
#include "ops/diff.h"

using namespace hs;

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (cond) ++g_passed;                                                           \
    else {                                                                          \
      ++g_failed;                                                                   \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n";    \
    }                                                                               \
  } while (0)

struct Test {
  const char* name;
  std::function<void()> fn;
};
static std::vector<Test>& tests() {
  static std::vector<Test> t;
  return t;
}
struct Register {
  Register(const char* n, std::function<void()> f) { tests().push_back({n, std::move(f)}); }
};
#define TEST(name)                                     \
  static void test_##name();                           \
  static Register reg_##name(#name, test_##name);      \
  static void test_##name()

static fs::path temp_dir(const std::string& name) {
  fs::path p = fs::temp_directory_path() / ("hs_test_" + name + "_" + new_uuid().substr(0, 8));
  fs::create_directories(p);
  return p;
}

TEST(hash_is_stable) {
  CHECK(hash_bytes("def", "abc") == hash_bytes("def", "abc"));
  CHECK(hash_bytes("def", "abc") != hash_bytes("file", "abc"));
  CHECK(hash_bytes("x", "").size() == 52);
}

TEST(dedent_indent) {
  std::string prefix;
  CHECK(dedent("    a\n\n      b\n", &prefix) == "a\n\n  b\n");
  CHECK(prefix == "    ");
  CHECK(indent("a\n\n  b\n", "    ") == "    a\n\n      b\n");
}

TEST(glob) {
  CHECK(glob_match("*.py", "a.py"));
  CHECK(!glob_match("*.py", "d/a.py"));
  CHECK(glob_match("**/*.py", "d/e/a.py"));
  CHECK(glob_match("data/**", "data/x/y"));
}

TEST(python_split) {
  const Language* py = language_by_name("python");
  std::string src = read_file(fs::path(HS_FIXTURES) / "py" / "sample.py");
  auto chunks = py->split(src);
  std::vector<std::string> names;
  for (auto& c : chunks) names.push_back(c.name);
  std::vector<std::string> want = {"greet", "decorated", "Server", "fetch", "one_liner"};
  CHECK(names == want);
  // greet takes its two comment lines
  CHECK(starts_with(src.substr(chunks[0].begin), "# A comment attached to greet."));
  // Server has its methods and nested class as children
  std::vector<std::string> kids;
  for (auto& c : chunks[2].children) kids.push_back(c.name);
  std::vector<std::string> want_kids = {"__init__", "address", "start", "prepare", "Config"};
  CHECK(kids == want_kids);
  CHECK(chunks[2].children[0].kind == "method");
  CHECK(chunks[2].children[1].kind == "method");
  CHECK(starts_with(src.substr(chunks[2].children[1].begin), "    # Comment above start."));
  CHECK(chunks[2].children[4].children.size() == 1);
}

TEST(python_analyze) {
  const Language* py = language_by_name("python");
  auto a = py->analyze("    def f(self, x):\n        \"\"\"Doc line.\"\"\"\n        return self.g(x) + h(1)\n");
  CHECK(a.signature == "def f(self, x)");
  CHECK(a.doc == "Doc line.");
  CHECK(a.calls.size() == 2);
  // Same tokens, different formatting and comments: same normalized hash.
  auto b = py->analyze("def f(self,x):  # c\n    \"\"\"Doc line.\"\"\"\n    return self.g( x )+h(1)\n");
  auto c = py->analyze("def f(self, x):\n    \"\"\"Doc line.\"\"\"\n    return self.g(x) - h(1)\n");
  auto a2 = py->analyze("def f(self, x):\n    \"\"\"Doc line.\"\"\"\n    return self.g(x) + h(1)\n");
  CHECK(a2.norm == b.norm);
  CHECK(a2.norm != c.norm);
}

TEST(cpp_split) {
  const Language* cpp = language_by_name("cpp");
  std::string src = read_file(fs::path(HS_FIXTURES) / "cpp" / "sample.hpp");
  auto chunks = cpp->split(src);
  std::vector<std::string> names;
  for (auto& c : chunks) names.push_back(c.kind + " " + c.name);
  if (std::getenv("HS_DEBUG")) {
    std::function<void(const std::vector<Chunk>&, int)> dump = [&](const std::vector<Chunk>& v, int d) {
      for (auto& c : v) {
        std::cerr << std::string(d * 2, ' ') << c.kind << " " << c.name << "\n";
        dump(c.children, d + 1);
      }
    };
    dump(chunks, 0);
  }
  CHECK((names == std::vector<std::string>{"namespace demo", "struct Anonymous", "struct Named"}));
  std::vector<std::string> kids;
  for (auto& c : chunks[0].children) kids.push_back(c.kind + " " + c.name);
  CHECK((kids == std::vector<std::string>{"struct Point", "class Shape", "enum Color", "function twice",
                                          "function add", "function add", "method Shape::normalize"}));
  // Comment above Point is attached; class members split out.
  CHECK(starts_with(src.substr(chunks[0].children[0].begin), "// A point in 2D."));
  std::vector<std::string> members;
  for (auto& c : chunks[0].children[1].children) members.push_back(c.name);
  CHECK((members == std::vector<std::string>{"~Shape", "scale"}));
  auto a = cpp->analyze(src.substr(chunks[0].children[6].begin, chunks[0].children[6].end - chunks[0].children[6].begin));
  CHECK(a.signature == "void Shape::normalize()");
  CHECK(a.calls.size() == 1 && a.calls[0].name == "helper::clamp");
}

static bool roundtrip_file(Store& s, const std::string& path, const std::string& bytes) {
  FileEntry e = store_file(s, path, bytes);
  return render_entry(s, e) == bytes;
}

TEST(roundtrip_fixtures) {
  fs::path dir = temp_dir("rt");
  {
    Store s(to_generic(dir / "store.db"));
    std::string src = read_file(fs::path(HS_FIXTURES) / "py" / "sample.py");
    CHECK(roundtrip_file(s, "sample.py", src));
    // CRLF line endings and no trailing newline
    std::string crlf;
    for (char c : src) {
      if (c == '\n') crlf += '\r';
      crlf += c;
    }
    CHECK(roundtrip_file(s, "crlf.py", crlf));
    CHECK(roundtrip_file(s, "nonl.py", "def a():\n    return 1"));
    CHECK(roundtrip_file(s, "empty.py", ""));
    CHECK(roundtrip_file(s, "broken.py", "def a(:\n  pass\nclass\n\ndef b():\n    return 2\n"));
    CHECK(roundtrip_file(s, "bin.dat", std::string("\x00\x01\xff", 3)));
  }
  fs::remove_all(dir);
}

TEST(import_render_project) {
  fs::path dir = temp_dir("proj");
  {
    auto repo = Repo::init(dir, "t");
    auto r = import_path(*repo, fs::path(HS_FIXTURES) / "py", "pkg");
    CHECK(r.files == 2);
    CHECK(r.defs >= 10);
    fs::path out = dir / "out";
    auto rr = render_to(*repo, out);
    CHECK(rr.written == 2);
    CHECK(read_file(out / "pkg" / "sample.py") == read_file(fs::path(HS_FIXTURES) / "py" / "sample.py"));
    auto rr2 = render_to(*repo, out);
    CHECK(rr2.written == 0 && rr2.unchanged == 2);
  }
  fs::remove_all(dir);
}

TEST(edit_keeps_identity) {
  fs::path dir = temp_dir("edit");
  {
    auto repo = Repo::init(dir, "t");
    import_path(*repo, fs::path(HS_FIXTURES) / "sample_project");
    autolink_tests(*repo);
    IndexRow add = resolve_name(*repo, "calc/ops.py:add");
    CHECK(!tests_info(*repo, add.qname)["tests"].empty());

    // Note on line 2, then rename add -> plus with a new first line.
    Namespace ns = repo->work_ns();
    add_note(*repo, ns, add.qname, 2, 2, "the sum");
    repo->set_work_ns(ns, "note");
    ns = repo->work_ns();
    auto o = replace_def(*repo, ns, add.qname, "def plus(a, b):\n    \"\"\"Sum.\"\"\"\n    return a + b\n",
                         short_hash(add.hash), "rename");
    CHECK(o.ok);
    commit_edits(*repo, ns, {&o});
    IndexRow plus = resolve_name(*repo, "calc/ops.py:plus");
    CHECK(plus.lineage == add.lineage);                       // same identity
    CHECK(!tests_info(*repo, plus.qname)["tests"].empty());   // tests followed
    Attach at = attach_of(repo->store(), repo->work_ns(), plus.lineage);
    CHECK(at.notes.size() == 1 && at.notes[0].from == 3);     // note moved down a line
    CHECK(at.context.size() == 1 && at.context[0].kind == "change");

    // Stale base hash is refused.
    ns = repo->work_ns();
    auto stale = replace_def(*repo, ns, plus.qname, "def plus(a, b):\n    return b + a\n", short_hash(add.hash));
    CHECK(!stale.ok);

    // A method edit keeps the class's other members; add and delete work.
    ns = repo->work_ns();
    auto m = add_def(*repo, ns, "calc/ops.py", "Calculator", "apply", "def peek(self):\n    return self.memory\n");
    CHECK(m.ok);
    auto d = delete_def(*repo, ns, "calc/ops.py:Calculator.clear");
    CHECK(d.ok);
    commit_edits(*repo, ns, {&m, &d});
    auto names = list_defs(*repo, "calc/ops.py:Calculator.");
    std::vector<std::string> got;
    for (auto& r : names) got.push_back(r.qname.substr(r.qname.rfind('.') + 1));
    CHECK((got == std::vector<std::string>{"__init__", "apply", "peek"}));
    std::string text = render_def(repo->store(), resolve_name(*repo, "Calculator").hash);
    CHECK(text.find("    def peek(self):\n        return self.memory\n") != std::string::npos);
  }
  fs::remove_all(dir);
}

// Verify after commit must still run the tests linked to what a push would send,
// and a failing test must fail verification. Needs python with pytest.
TEST(verify_runs_python_tests) {
  if (run_command(fs::current_path(), "python -m pytest --version").code != 0) {
    std::cerr << "  (skipped: python -m pytest not available)\n";
    return;
  }
  fs::path dir = temp_dir("verify");
  {
    auto repo = Repo::init(dir, "t");
    import_path(*repo, fs::path(HS_FIXTURES) / "sample_project");
    autolink_tests(*repo);
    commit(*repo, "import");
    auto r = verify(*repo, false);
    CHECK(r["status"] == "passed");
    CHECK(r["tests_run"].size() == 4);
    {
      std::set<std::string> one{"calc/ops.py:mul"};
      auto sums = test_summaries(*repo, repo->work_ns(), &one);
      CHECK(sums.count("calc/ops.py:mul") && sums["calc/ops.py:mul"].total >= 1 &&
            sums["calc/ops.py:mul"].passed == sums["calc/ops.py:mul"].total);
      CHECK(!r["functions"].empty());
    }
    // As if pushed: later verifies review only what the hub has not seen.
    repo->store().set_ref("remote/origin/main", *repo->head_commit());

    // Break add. Its own tests fail, and so does mul's test, because mul calls
    // add: verify follows callers through the graph.
    Namespace ns = repo->work_ns();
    IndexRow add = resolve_name(*repo, "calc/ops.py:add");
    auto o = replace_def(*repo, ns, add.qname, "def add(a, b):\n    return a - b\n", short_hash(add.hash), "break");
    CHECK(o.ok);
    commit_edits(*repo, ns, {&o});
    commit(*repo, "break add");
    auto bad = verify(*repo, false);
    if (bad["status"] != "failed") std::cerr << bad.dump(2) << "\n";
    CHECK(bad["status"] == "failed");
    bool mul_failed = false;
    for (auto& t : bad["tests_run"])
      if (t["test"] == "tests/test_ops.py:test_mul") mul_failed = t["status"] == "failed";
    CHECK(mul_failed);
    CHECK(bad["impacted"].size() == 1 && bad["impacted"][0] == "calc/ops.py:mul");
    {
      auto sums = test_summaries(*repo, repo->work_ns());
      CHECK(sums["calc/ops.py:add"].failed >= 1);
      CHECK(sums["calc/ops.py:add"].label().find("failed") != std::string::npos);
    }
  }
  fs::remove_all(dir);
}

// Calls the hub service directly, as the HTTP server would.
struct InProcessHub : HubTransport {
  HubService& svc;
  std::string project;
  InProcessHub(HubService& s, std::string p) : svc(s), project(std::move(p)) {}
  // Sends the process's access key, as the HTTP transport does.
  nlohmann::json call(const std::string& op, const nlohmann::json& req) override {
    return svc.handle(project, op, req, access_key());
  }
};

TEST(hub_sync) {
  fs::path dir = temp_dir("hub");
  {
    auto backend = make_sqlite_backend(to_generic(dir / "hub"));
    HubService svc(*backend);
    svc.handle("demo", "create", {});
    InProcessHub hub(svc, "demo");
    set_lock_transport_override(&hub);

    // Agent A imports, commits, and must verify before pushing.
    auto a = Repo::init(dir / "a", "demo");
    a->set_agent("agent-a");
    set_remote(*a, "origin", "http://unused/demo");
    import_path(*a, fs::path(HS_FIXTURES) / "sample_project");
    autolink_tests(*a);
    commit(*a, "import");
    bool refused = false;
    try {
      push(*a, hub);
    } catch (std::exception&) {
      refused = true;
    }
    CHECK(refused);  // not verified
    a->store().exec("INSERT INTO verification(ns, status, time, agent, report) "
                    "SELECT value, 'passed', 0, 'test', '{}' FROM ref WHERE name = 'work/main'");
    auto pr = push(*a, hub);
    CHECK(pr["uploaded"].get<int>() > 10);

    // Agent B clones it.
    auto b = Repo::init(dir / "b", "demo");
    b->set_agent("agent-b");
    set_remote(*b, "origin", "http://unused/demo");
    auto pl = pull(*b, hub);
    CHECK(b->work_ns_hash() == a->work_ns_hash());
    CHECK(render_def(b->store(), resolve_name(*b, "mul").hash) == render_def(a->store(), resolve_name(*a, "mul").hash));

    // A locks mul: the lock goes to the hub, and B can't take it.
    IndexRow mul = resolve_name(*a, "mul");
    acquire_lock(*a, mul.lineage, mul.qname);
    bool blocked = false;
    try {
      acquire_lock(*b, mul.lineage, mul.qname);
    } catch (std::exception&) {
      blocked = true;
    }
    CHECK(blocked);
    // After a pull, B's local edits respect the hub lock too.
    pull(*b, hub);
    Namespace nsb = b->work_ns();
    auto eb = replace_def(*b, nsb, mul.qname, "def mul(a, b):\n    return a * b\n", "");
    CHECK(!eb.ok);

    // A edits, commits, verifies, pushes; B fast-forwards.
    Namespace nsa = a->work_ns();
    auto ea = replace_def(*a, nsa, mul.qname, "def mul(a, b):\n    return a * b\n", "", "simpler");
    CHECK(ea.ok);
    commit_edits(*a, nsa, {&ea});
    commit(*a, "simpler mul");
    a->store().exec("INSERT OR IGNORE INTO verification(ns, status, time, agent, report) "
                    "SELECT value, 'passed', 0, 'test', '{}' FROM ref WHERE name = 'work/main'");
    push(*a, hub);
    auto pl2 = pull(*b, hub);
    CHECK(starts_with(pl2["result"].get<std::string>(), "fast-forward"));
    CHECK(render_def(b->store(), resolve_name(*b, "mul").hash) == "def mul(a, b):\n    return a * b\n");
    // Commit released A's lock locally; the next push synced that to the hub.
    CHECK(svc.handle("demo", "locks", {})["locks"].empty());

    // A stale push (B behind) is refused rather than overwriting.
    Namespace nsb2 = b->work_ns();
    auto eb2 = add_def(*b, nsb2, "calc/ops.py", "", "", "def neg(a):\n    return -a\n");
    commit_edits(*b, nsb2, {&eb2});
    commit(*b, "neg");
    Namespace nsa2 = a->work_ns();
    auto ea2 = add_def(*a, nsa2, "calc/ops.py", "", "", "def sq(a):\n    return a * a\n");
    commit_edits(*a, nsa2, {&ea2});
    commit(*a, "sq");
    for (auto* r : {a.get(), b.get()})
      r->store().exec("INSERT OR IGNORE INTO verification(ns, status, time, agent, report) "
                      "SELECT value, 'passed', 0, 'test', '{}' FROM ref WHERE name = 'work/main'");
    push(*a, hub);
    bool stale = false;
    try {
      push(*b, hub);
    } catch (std::exception&) {
      stale = true;
    }
    CHECK(stale);

    // Hub publishes the branch to a git working copy.
    Repo* hub_repo = backend->project("demo", false)->native_repo();
    write_file(hub_repo->hs_dir() / "config.toml",
               read_file(hub_repo->hs_dir() / "config.toml") + "\n[publish]\ngit_dir = \"git-out\"\n");
    auto hub_repo2 = Repo::open(hub_repo->root());
    auto pub = publish(*hub_repo2);
    CHECK(pub["changed"].get<bool>());
    CHECK(read_file(hub_repo->root() / "git-out" / "calc" / "ops.py").find("def sq(a):") != std::string::npos);
    auto pub2 = publish(*hub_repo2);
    CHECK(!pub2["changed"].get<bool>());
    set_lock_transport_override(nullptr);
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(diff_shows_module_code_and_removed_text) {
  fs::path dir = temp_dir("diffmod");
  {
    fs::path src = dir / "src";
    fs::copy(fs::path(HS_FIXTURES) / "sample_project", src, fs::copy_options::recursive);
    auto repo = Repo::init(dir / "p", "t");
    import_path(*repo, src);
    commit(*repo, "import");
    Hash before = repo->work_ns_hash();
    // Module-level change: a new constant next to the definitions.
    std::string ops = read_file(src / "calc" / "ops.py");
    write_file(src / "calc" / "ops.py", ops.replace(ops.find("\"\"\"Arithmetic helpers.\"\"\"\n"), 26,
                                                     "\"\"\"Arithmetic helpers.\"\"\"\n\nLIMIT = 100\n"));
    import_path(*repo, src);
    // Removed definition.
    Namespace ns = repo->work_ns();
    auto o = delete_def(*repo, ns, "calc/ops.py:mul");
    CHECK(o.ok);
    commit_edits(*repo, ns, {&o});
    Store& s = repo->store();
    auto d = diff_namespaces(s, load_namespace(s, before), repo->work_ns());
    auto j = diff_to_json(s, d, true);
    bool removed_text = false, module_diff = false;
    for (auto& c : j["definitions"])
      if (c["change"] == "removed" && c.value("removed_text", "").find("total = add(total, a)") != std::string::npos)
        removed_text = true;
    for (auto& m : j["module_code"])
      if (m["file"] == "calc/ops.py" && m.value("diff", "").find("+LIMIT = 100") != std::string::npos) module_diff = true;
    if (!module_diff || !removed_text) std::cerr << j.dump(2) << "\n";
    CHECK(removed_text);
    CHECK(module_diff);
    // A cap lists the rest without text.
    auto capped = diff_to_json(s, d, true, 1);
    CHECK(capped.contains("truncated"));
  }
  fs::remove_all(dir);
}

TEST(review_flags_and_sync) {
  fs::path dir = temp_dir("review");
  {
    auto backend = make_sqlite_backend(to_generic(dir / "hub"));
    HubService svc(*backend);
    svc.handle("demo", "create", {});
    InProcessHub hub(svc, "demo");
    set_lock_transport_override(&hub);

    auto a = Repo::init(dir / "a", "demo");
    a->set_agent("agent-a");
    import_path(*a, fs::path(HS_FIXTURES) / "sample_project");
    commit(*a, "import");
    Hash c1 = *a->head_commit();
    CHECK(review_status(a->store(), c1).level == "unreviewed");

    // An AI review sets the flag to "ai"; a person's review to "human".
    add_review(*a, "head", "approved", "", "ai");
    CHECK(review_status(a->store(), c1).level == "ai");
    CHECK(!review_status(a->store(), c1).human_reviewed);
    bool refused = false;
    try {
      add_review(*a, "head", "changes_requested", "", "human");  // needs a summary
    } catch (std::exception&) {
      refused = true;
    }
    CHECK(refused);
    add_review(*a, "head", "approve", "", "human", nlohmann::json::array(), "Richard");
    auto st = review_status(a->store(), c1);
    CHECK(st.level == "human" && st.human_approved && st.label() == "human approved");

    // Push carries the reviews; the hub has the same flag.
    a->store().exec("INSERT INTO verification(ns, status, time, agent, report) "
                    "SELECT value, 'passed', 0, 'test', '{}' FROM ref WHERE name = 'work/main'");
    auto pr = push(*a, hub);
    CHECK(pr["reviews_sent"] == 2);
    CHECK(pr["commits"] == 1 && pr["human_reviewed"] == 1);
    Repo* h = backend->project("demo", false)->native_repo();
    CHECK(review_status(h->store(), c1).level == "human");
    CHECK(push_log(h->store(), 10).size() == 1);

    // A review made on the hub (its pages) reaches a clone on pull.
    add_review(*h, c1, "comment", "looks fine to me too", "human", nlohmann::json::array(), "Sam");
    auto b = Repo::init(dir / "b", "demo");
    b->set_agent("agent-b");
    auto pl = pull(*b, hub, "origin", "main");
    CHECK(pl["reviews_fetched"] == 3);
    CHECK(reviews_of(b->store(), c1).size() == 3);
    CHECK(review_status(b->store(), c1).human_approved);
    auto again = pull(*b, hub, "origin", "main");
    CHECK(again["reviews_fetched"] == 0);  // incremental

    // show_commit has the diff, the flag and the reviews.
    auto show = show_commit(*b, c1, 0);
    CHECK(show["review"]["level"] == "human");
    CHECK(show["reviews"].size() == 3);
    CHECK(!show["diff"]["definitions"].empty());
    set_lock_transport_override(nullptr);
  }
  fs::remove_all(dir);
}

static void mark_verified(Repo& r) {
  r.store().exec("INSERT OR IGNORE INTO verification(ns, status, time, agent, report) "
                 "SELECT value, 'passed', 0, 'test', '{}' FROM ref WHERE name = 'work/main'");
}

static bool throws(const std::function<void()>& f) {
  try {
    f();
  } catch (std::exception&) {
    return true;
  }
  return false;
}

static bool has_lock(const nlohmann::json& locks, const std::string& target, const std::string& holder,
                     const std::string& work) {
  for (auto& l : locks)
    if (l["target"] == target && l["holder"] == holder && l["work"] == work) return true;
  return false;
}

// Two agents through a hub: a change request on a locked function is made by
// the holder before it commits, with the requester's test; a lock request is
// granted when the holder lets go.
TEST(work_requests_over_hub) {
  fs::path dir = temp_dir("work_hub");
  {
    auto backend = make_sqlite_backend(to_generic(dir / "hub"));
    HubService svc(*backend);
    svc.handle("demo", "create", {});
    InProcessHub hub(svc, "demo");
    set_lock_transport_override(&hub);

    auto a = Repo::init(dir / "a", "demo");
    a->set_agent("agent-a");
    set_remote(*a, "origin", "http://unused/demo");
    import_path(*a, fs::path(HS_FIXTURES) / "sample_project");
    commit(*a, "import");
    mark_verified(*a);
    push(*a, hub);
    auto b = Repo::init(dir / "b", "demo");
    b->set_agent("agent-b");
    set_remote(*b, "origin", "http://unused/demo");
    pull(*b, hub);

    WorkItem fa;
    fa.id = "FEAT-12";
    fa.title = "simpler arithmetic";
    start_work(*a, fa);
    WorkItem fb;
    fb.id = "BUG-7";
    fb.kind = "bug";
    fb.title = "mul with negative numbers";
    start_work(*b, fb);
    CHECK(coordination(*b)->work("BUG-7")->priority == 2);  // bugs go first by default

    // A locks mul for FEAT-12; everyone can see what the lock is for.
    IndexRow mul = resolve_name(*a, "mul");
    acquire_lock(*a, mul.lineage, mul.qname);
    CHECK(has_lock(svc.handle("demo", "locks", {})["locks"], mul.lineage, "agent-a", "FEAT-12"));
    CHECK(throws([&] { acquire_lock(*b, mul.lineage, mul.qname); }));

    // B doesn't wait: it asks A for the change, with a test that must pass.
    std::string test_code = "def test_mul_negative():\n    assert mul(-2, 3) == -6\n";
    Request rq = make_request(*b, "mul", "change", "make mul work for a negative multiplicand", "tests/test_ops.py",
                              test_code);
    CHECK(rq.holder == "agent-a" && rq.holder_work == "FEAT-12" && rq.priority == 2 && rq.status == "open");
    auto ea = inbox(*a, true);
    CHECK(ea.size() == 1 && ea[0].kind == "request_new");
    CHECK(inbox(*a, true).empty());  // the cursor moved

    // A changes mul; committing it with B's request still open is refused.
    Namespace ns = a->work_ns();
    auto e = replace_def(*a, ns, mul.qname, "def mul(a, b):\n    return a * b\n", "", "simpler");
    CHECK(e.ok);
    commit_edits(*a, ns, {&e});
    CHECK(throws([&] { commit_work(*a, "simpler mul"); }));

    // A accepts: B's test becomes one of mul's tests, so verify runs it.
    accept_request(*a, rq.id);
    CHECK(tests_info(*a, mul.qname).dump().find("test_mul_negative") != std::string::npos);
    auto eb = inbox(*b, true);
    CHECK(!eb.empty() && eb.back().kind == "request_accepted");
    auto wc = commit_work(*a, "simpler mul; works for negatives too", {rq.id});
    CHECK(wc.resolved.size() == 1 && wc.landed == 0);
    CHECK(coordination(*a)->request(rq.id)->status == "resolved");  // made, not shared yet

    // The push brings the commit to the hub: done, and B is told.
    mark_verified(*a);
    push(*a, hub);
    CHECK(coordination(*a)->request(rq.id)->status == "done");
    auto eb2 = inbox(*b, true);
    CHECK(!eb2.empty() && eb2.back().kind == "request_done");
    CHECK(describe_event(eb2.back()).find("pull") != std::string::npos);
    pull(*b, hub);
    CHECK(render_def(b->store(), resolve_name(*b, "mul").hash) == "def mul(a, b):\n    return a * b\n");

    // Lock queue: B waits for add; when A lets go, the lock is B's.
    IndexRow add = resolve_name(*a, "add");
    acquire_lock(*a, add.lineage, add.qname);
    Request q = make_request(*b, "add", "lock", "");
    CHECK(q.kind == "lock" && q.holder == "agent-a");
    release_lock(*a, add.lineage);
    CHECK(has_lock(svc.handle("demo", "locks", {})["locks"], add.lineage, "agent-b", "BUG-7"));
    CHECK(coordination(*b)->request(q.id)->status == "granted");
    auto eb3 = inbox(*b, true);
    CHECK(!eb3.empty() && eb3.back().kind == "lock_granted");
    pull(*b, hub);  // brings the lock handed to B into its own store
    auto lb = lock_on(*b, add.lineage);
    CHECK(lb && lb->holder == "agent-b");
    set_lock_transport_override(nullptr);
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// Several agents on one project without a hub: committing is sharing, an
// expired lease hands the lock on, and a declined request queues for the lock.
TEST(work_requests_local) {
  fs::path dir = temp_dir("work_local");
  {
    auto r = Repo::init(dir / "p", "demo");
    r->set_agent("agent-a");
    import_path(*r, fs::path(HS_FIXTURES) / "sample_project");
    commit(*r, "import");
    WorkItem wa;
    wa.id = "FEAT-1";
    wa.title = "faster mul";
    start_work(*r, wa);
    IndexRow mul = resolve_name(*r, "mul");
    acquire_lock(*r, mul.lineage, mul.qname);

    r->set_agent("agent-b");
    CHECK(current_work_id(*r) == "adhoc-agent-b");  // no item yet: an ad-hoc one
    // A checkout that finds it locked, with a need, logs the need to the holder.
    CHECK(throws([&] { make_request(*r, "mul", "change", "keep floats"); }));  // too thin for another agent to act on
    Request rq;
    try {
      acquire_lock(*r, mul.lineage, mul.qname);
    } catch (LockedError& e) {
      CHECK(e.held.holder == "agent-a" && e.held.work == "FEAT-1" && e.name == mul.qname);
      Need n;
      n.text = "mul must keep accepting float arguments: mul(1.5, 2) returns 3.0 and mul(2, 0.5) returns 1.0";
      rq = request_from_blocked_checkout(*r, e, n);
    }
    CHECK(rq.id > 0 && rq.holder == "agent-a" && rq.holder_work == "FEAT-1");
    CHECK(rq.requester_work == "adhoc-agent-b");
    CHECK(rq.context.value("version", "") == mul.hash);  // the version the requester saw
    CHECK(request_details(rq).find("mul(1.5, 2) returns 3.0") != std::string::npos);

    r->set_agent("agent-a");
    Namespace ns = r->work_ns();
    auto e = replace_def(*r, ns, mul.qname, "def mul(a, b):\n    return a * b\n", "", "faster");
    commit_edits(*r, ns, {&e});
    CHECK(throws([&] { commit_work(*r, "faster mul"); }));
    CHECK(throws([&] { commit_work(*r, "faster mul", {rq.id + 99}); }));
    try {
      commit_work(*r, "faster mul");
    } catch (std::exception& e) {  // the refusal carries the requester's whole brief
      CHECK(std::string(e.what()).find("mul(2, 0.5) returns 1.0") != std::string::npos);
    }
    auto wc = commit_work(*r, "faster mul, floats still work", {rq.id});
    CHECK(wc.landed == 0);  // not done at commit: only once verify passes with its tests
    CHECK(coordination(*r)->request(rq.id)->status == "resolved");
    CHECK(!lock_on(*r, mul.lineage));
    CHECK(land_verified(*r) == 1);  // what a passing hs verify does (no hub)
    CHECK(coordination(*r)->request(rq.id)->status == "done");

    r->set_agent("agent-b");
    auto ev = inbox(*r, true);
    CHECK(!ev.empty() && ev.back().kind == "request_done");

    // A lease that runs out hands the lock to whoever queued for it.
    r->set_agent("agent-a");
    IndexRow add = resolve_name(*r, "add");
    acquire_lock(*r, add.lineage, add.qname);
    r->set_agent("agent-b");
    make_request(*r, "add", "lock", "");
    r->store().exec("UPDATE lock SET expires = 1");
    bool handed = false;
    for (auto& l : coordination(*r)->locks()) handed |= l.target == add.lineage && l.holder == "agent-b";
    CHECK(handed);

    // B holds add now. A asks for a change, B declines: A is queued for the
    // lock instead, and gets it when B lets go.
    r->set_agent("agent-a");
    Request r2 = make_request(*r, "add", "change", "rename the arguments of add to left and right, keeping their order");
    r->set_agent("agent-b");
    CHECK(throws([&] { decline_request(*r, r2.id, ""); }));  // a reason is required
    decline_request(*r, r2.id, "out of scope for my change");
    release_lock(*r, add.lineage);
    auto l = lock_on(*r, add.lineage);
    CHECK(l && l->holder == "agent-a");
    r->set_agent("agent-a");
    auto ea = inbox(*r, true);
    CHECK(ea.size() >= 2 && ea[ea.size() - 2].kind == "request_declined" && ea.back().kind == "lock_granted");
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// The hub working the way it would on any non-SQLite backend: a cached
// workspace built only from the backend interfaces serves the MCP tools and
// publishing, and writes its changes back.
TEST(hub_cached_workspace) {
  fs::path dir = temp_dir("hub_cache");
  {
    auto backend = make_sqlite_backend(to_generic(dir / "hub"));
    HubOptions opt;
    opt.force_cache = true;
    opt.cache_dir = to_generic(dir / "cache");
    HubService svc(*backend, opt);
    svc.handle("demo", "create", {});
    InProcessHub hub(svc, "demo");
    set_lock_transport_override(&hub);

    auto a = Repo::init(dir / "a", "demo");
    a->set_agent("agent-a");
    set_remote(*a, "origin", "http://unused/demo");
    import_path(*a, fs::path(HS_FIXTURES) / "sample_project");
    commit(*a, "import");
    mark_verified(*a);
    push(*a, hub);

    Workspace* ws = svc.workspace("demo");
    CHECK(ws && ws->cached());
    Repo* native = backend->project("demo", false)->native_repo();
    CHECK(ws->repo().root() != native->root());
    CHECK(ws->repo().work_ns_hash() == a->work_ns_hash());
    CHECK(resolve_name(ws->repo(), "mul").qname == "calc/ops.py:mul");

    // An agent using the hub's MCP endpoint edits and commits in the cache...
    ws->repo().set_agent("web-agent");
    McpServer mcp(ws->repo());
    auto call = [&](const std::string& tool, const nlohmann::json& args) {
      return mcp.handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                         {"params", {{"name", tool}, {"arguments", args}}}});
    };
    auto r1 = call("hs_add_def", {{"file", "calc/ops.py"}, {"code", "def neg(a):\n    return -a\n"}, {"reason", "negation"}});
    CHECK(r1["result"]["isError"] == false);
    auto r2 = call("hs_commit", {{"message", "add neg"}});
    CHECK(r2["result"]["isError"] == false);
    ws->save("web-agent");
    ws->repo().set_agent("hub");
    // ...and the backend has it, so a client pulls it.
    CHECK(backend->project("demo", false)->branches()["main"] == *ws->repo().head_commit());
    auto pl = pull(*a, hub);
    CHECK(starts_with(pl["result"].get<std::string>(), "fast-forward"));
    CHECK(resolve_name(*a, "neg").qname == "calc/ops.py:neg");

    // Publishing runs as a job on the cached workspace; its config comes from the backend.
    fs::path git_out = dir / "git-out";
    write_file(native->hs_dir() / "config.toml", read_file(native->hs_dir() / "config.toml") +
                                                     "\n[publish]\ngit_dir = \"" + to_generic(git_out) + "\"\n");
    backend->enqueue({0, "demo", "publish", "main"});
    CHECK(svc.run_jobs() == 1);
    CHECK(fs::exists(git_out / "calc" / "ops.py") &&
          read_file(git_out / "calc" / "ops.py").find("def neg(a):") != std::string::npos);
    set_lock_transport_override(nullptr);
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// People sign in and make an orchestrator key; the orchestrator issues worker
// keys; on a project with keys, changes need one, a worker acts as its work
// item inside its scope, and a fresh agent with a new key carries on.
TEST(access_keys) {
  fs::path dir = temp_dir("access");
  {
    auto backend = make_sqlite_backend(to_generic(dir / "hub"));
    HubService svc(*backend);
    svc.handle("demo", "create", {});
    InProcessHub hub(svc, "demo");
    set_lock_transport_override(&hub);
    Access& acc = svc.access();

    // An agent imports before the project has keys.
    auto a = Repo::init(dir / "a", "demo");
    a->set_agent("agent-a");
    set_remote(*a, "origin", "http://unused/demo");
    import_path(*a, fs::path(HS_FIXTURES) / "sample_project");
    commit(*a, "import");
    mark_verified(*a);
    push(*a, hub);

    // A person signs in (accounts come from the hub machine) and makes an orchestrator key.
    CHECK(!acc.has_users());
    CHECK(throws([&] { acc.add_user("rich", "short"); }));
    acc.add_user("rich", "correct horse battery");
    CHECK(throws([&] { acc.sign_in("rich", "wrong password!"); }));
    CHECK(throws([&] { acc.sign_in("nobody", "correct horse battery"); }));
    std::string session = acc.sign_in("rich", "correct horse battery");
    CHECK(acc.session_user(session).value_or("") == "rich");
    CHECK(!acc.session_user("forged").has_value());
    CHECK(throws([&] { acc.issue_orchestrator("demo", "", 30, ""); }));
    auto orch = acc.issue_orchestrator("demo", "rich", 30, "ork-1");
    CHECK(starts_with(orch.text, "hsk_") && acc.required("demo"));
    CHECK(backend->keys("demo")[0].dump().find(orch.text.substr(orch.text.rfind('_') + 1)) == std::string::npos);  // only a hash is kept
    acc.sign_out(session);
    CHECK(!acc.session_user(session).has_value());

    // Without a key, reading works but changing doesn't.
    IndexRow mul = resolve_name(*a, "mul");
    CHECK(throws([&] { acquire_lock(*a, mul.lineage, mul.qname); }));
    CHECK(svc.handle("demo", "refs", {})["branches"].contains("main"));
    CHECK(throws([&] { svc.handle("demo", "issue_key", {{"work", "FEAT-1"}, {"title", "x"}}, "hsk_bogus_123"); }));

    // The orchestrator issues a worker key for FEAT-1, limited to calc/ops.py.
    json issued = svc.handle("demo", "issue_key",
                             {{"work", "FEAT-1"}, {"title", "faster arithmetic"}, {"scope", "calc/ops.py"}, {"hours", 8}},
                             orch.text);
    std::string wkey = issued["text"];
    CHECK(svc.handle("demo", "whoami", {}, wkey)["work"] == "FEAT-1");
    CHECK(throws([&] { svc.handle("demo", "issue_key", {{"work", "FEAT-2"}, {"title", "y"}}, wkey); }));  // workers can't

    // A worker presenting the key acts as FEAT-1, whatever its own name.
    set_access_key(wkey);
    auto w1 = Repo::init(dir / "w1", "demo");
    w1->set_agent("claude-7f3a");
    set_remote(*w1, "origin", "http://unused/demo");
    pull(*w1, hub);
    apply_access_key(*w1);
    CHECK(w1->agent() == "FEAT-1" && current_work_id(*w1) == "FEAT-1");
    acquire_lock(*w1, mul.lineage, mul.qname);
    CHECK(has_lock(svc.handle("demo", "locks", {})["locks"], mul.lineage, "FEAT-1", "FEAT-1"));
    IndexRow tst = resolve_name(*w1, "tests/test_ops.py:test_mul");
    CHECK(throws([&] { acquire_lock(*w1, tst.lineage, tst.qname); }));  // outside the key's scope
    // A forged identity in the request is replaced by the key's.
    svc.handle("demo", "unlock", {{"target", mul.lineage}, {"agent", "someone-else"}, {"force", true}}, wkey);
    CHECK(svc.handle("demo", "locks", {})["locks"].empty());
    acquire_lock(*w1, mul.lineage, mul.qname);

    // The agent is replaced (fresh context, new clone). The orchestrator gives
    // the new one a key for the same work item: it carries on with the lock.
    json issued2 = svc.handle("demo", "issue_key", {{"work", "FEAT-1"}, {"scope", "calc/**"}}, orch.text);
    set_access_key(issued2["text"]);
    auto w2 = Repo::init(dir / "w2", "demo");
    set_remote(*w2, "origin", "http://unused/demo");
    pull(*w2, hub);
    apply_access_key(*w2);
    CHECK(w2->agent() == "FEAT-1");
    CHECK(!blocking_lock(*w2, mul.lineage));  // FEAT-1's lock is its own
    Namespace ns = w2->work_ns();
    auto e = replace_def(*w2, ns, mul.qname, "def mul(a, b):\n    return a * b\n", "", "simpler");
    CHECK(e.ok);
    commit_edits(*w2, ns, {&e});
    commit_work(*w2, "simpler mul");
    mark_verified(*w2);
    push(*w2, hub);
    CHECK(svc.handle("demo", "locks", {})["locks"].empty());

    // An expired key stops working; revoking the orchestrator key stops its workers.
    json k = backend->key(issued["key"]["id"].get<std::string>());
    k["expires"] = 1;
    backend->put_key(k);
    CHECK(throws([&] { svc.handle("demo", "whoami", {}, wkey); }));
    CHECK(svc.handle("demo", "whoami", {}, issued2["text"].get<std::string>())["role"] == "worker");
    acc.revoke("demo", orch.key.id, "rich", nullptr);
    CHECK(throws([&] { svc.handle("demo", "whoami", {}, issued2["text"].get<std::string>()); }));
    CHECK(Access::in_scope("calc/**,docs/*", "calc/sub/x.py:f") && !Access::in_scope("calc/*.py", "tests/t.py:f"));
    set_access_key("");
    set_lock_transport_override(nullptr);
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// Git-ignore rules live in the namespace: committed and synced like code,
// rendered locally, and left out (or removed) when publishing to git.
TEST(git_ignore_rules) {
  fs::path dir = temp_dir("gitignore");
  {
    auto r = Repo::init(dir / "p", "demo");
    import_path(*r, fs::path(HS_FIXTURES) / "sample_project");
    commit(*r, "import");
    write_file(r->hs_dir() / "config.toml", read_file(r->hs_dir() / "config.toml") + "\n[publish]\ngit_dir = \"git-out\"\n");
    auto r2 = Repo::open(r->root());
    auto pub = publish(*r2);
    fs::path out = r2->root() / "git-out";
    CHECK(fs::exists(out / "tests" / "test_ops.py"));
    Hash ns_before = r2->work_ns_hash();

    CHECK(throws([&] { git_ignore_add(*r2, "tests/", ""); }));  // a reason is required
    json added = git_ignore_add(*r2, "tests/", "test scaffolding stays internal");
    CHECK(added["pattern"] == "tests" && !added["files"].empty());
    CHECK(git_ignore_matches("*.env", "config/prod.env") && git_ignore_matches("config", "config/a/b.txt") &&
          !git_ignore_matches("calc/x.py", "calc/x.pyc") && !git_ignore_matches("test", "tests/a.py"));
    CHECK(r2->work_ns_hash() != ns_before);
    auto c = commit(*r2, "keep tests out of git");
    CHECK(load_commit(r2->store(), c.commit).changes.back() == "git-ignore rules changed");
    CHECK(load_namespace(r2->store(), r2->work_ns_hash()).git_ignore.at("tests") == "test scaffolding stays internal");

    auto pub2 = publish(*r2);
    CHECK(pub2["changed"].get<bool>() && pub2["git_ignored"].get<int>() >= 1);
    CHECK(!fs::exists(out / "tests" / "test_ops.py"));  // removed from the git branch
    CHECK(fs::exists(out / "calc" / "ops.py"));
    render_to(*r2, dir / "local");                       // a local render still has everything
    CHECK(fs::exists(dir / "local" / "tests" / "test_ops.py"));

    git_ignore_remove(*r2, "tests/");
    CHECK(git_ignore_list(*r2).empty());
    CHECK(r2->work_ns_hash() == ns_before);              // no rules: the same namespace hash as before
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// A Godot 4 project: GDScript functions, inner classes, properties and enums;
// scene nodes, resources and signal connections; project settings sections.
// Everything round-trips byte for byte, the editor cache is skipped, and
// references show in the graph.
TEST(godot_project) {
  fs::path dir = temp_dir("godot");
  {
    fs::path src = fs::path(HS_FIXTURES) / "godot_project";
    auto r = Repo::init(dir / "p", "game");
    auto res = import_path(*r, src);
    CHECK(res.skipped == 1);  // .godot/
    auto kind_of = [&](const std::string& q) {
      auto row = row_by_qname(*r, q);
      return row ? row->kind : std::string("(missing)");
    };
    CHECK(kind_of("player.gd:_physics_process") == "function");
    CHECK(kind_of("player.gd:take_damage") == "function");
    CHECK(kind_of("player.gd:_init") == "function");
    CHECK(kind_of("player.gd:make") == "function");
    CHECK(kind_of("player.gd:health") == "property");
    CHECK(kind_of("player.gd:State") == "enum");
    CHECK(kind_of("player.gd:Inventory") == "class");
    CHECK(kind_of("player.gd:Inventory.add") == "method");
    CHECK(kind_of("player.gd:SPEED") == "(missing)");  // constants stay module code
    CHECK(kind_of("scenes/player.tscn:Player") == "node");
    CHECK(kind_of("scenes/player.tscn:Hurtbox/Shape") == "node");
    CHECK(kind_of("scenes/player.tscn:Label") == "node");
    CHECK(kind_of("scenes/player.tscn:ext_1_k3j2h") == "ext_resource");
    CHECK(kind_of("scenes/player.tscn:RectangleShape2D_x7c2v") == "sub_resource");
    CHECK(kind_of("scenes/player.tscn:Hurtbox/body_entered-take_damage") == "connection");
    CHECK(kind_of("resources/stats.tres:resource") == "resource");
    CHECK(kind_of("project.godot:input") == "section");
    CHECK(list_defs(*r, "scenes/player.tscn").size() == 12);  // "[one]" inside the Label's text is not a section

    // Annotations and comments above a function belong to it; the file's @tool does not.
    std::string td = render_def(r->store(), resolve_name(*r, "player.gd:take_damage").hash);
    CHECK(starts_with(td, "@rpc(\"any_peer\")\nfunc take_damage"));
    std::string pp = render_def(r->store(), resolve_name(*r, "player.gd:_physics_process").hash);
    CHECK(starts_with(pp, "# Moves the player"));
    Namespace ns0 = r->work_ns();
    std::string glue = render_entry(r->store(), ns0.files.at("player.gd"));
    CHECK(starts_with(glue, "@tool\nclass_name Player"));

    // Byte-for-byte round trip of every file.
    render_to(*r, dir / "out");
    for (auto& rel : {"player.gd", "project.godot", "scenes/player.tscn", "resources/stats.tres",
                      "resources/stats.gd", "test/test_player.gd", "icon.svg", "icon.svg.import", "player.gd.uid"})
      CHECK(read_file(dir / "out" / rel) == read_file(src / rel));
    CHECK(!fs::exists(dir / "out" / ".godot"));

    // The graph: scene -> script and resources, connection -> the script's handler.
    json callees_root = callees(*r, "scenes/player.tscn:Player");
    bool uses_script = false;
    for (auto& c : callees_root) uses_script |= c.value("callee", "") == "scenes/player.tscn:ext_1_k3j2h";
    CHECK(uses_script);
    json handler = callees(*r, "scenes/player.tscn:Hurtbox/body_entered-take_damage");
    CHECK(!handler.empty() && handler[0].value("callee", "") == "player.gd:take_damage");

    // Editing one node changes that node only.
    commit(*r, "import");
    Namespace ns = r->work_ns();
    auto e = replace_def(*r, ns, "scenes/player.tscn:Sprite2D",
                         "[node name=\"Sprite2D\" type=\"Sprite2D\" parent=\".\"]\ntexture = ExtResource(\"2_m8f4d\")\nhframes = 4\n",
                         "", "four frames");
    CHECK(e.ok);
    auto e2 = replace_def(*r, ns, "player.gd:make", "static func make() -> Player:\n\tvar p := Player.new()\n\treturn p\n", "",
                          "named local");
    CHECK(e2.ok);
    commit_edits(*r, ns, {&e, &e2});
    auto d = diff_namespaces(r->store(), load_namespace(r->store(), r->head_ns_hash()), r->work_ns());
    CHECK(d.defs.size() == 2);
    std::set<std::string> changed;
    for (auto& x : d.defs) changed.insert(x.new_name);
    CHECK(changed.count("scenes/player.tscn:Sprite2D") && changed.count("player.gd:make"));
    render_to(*r, dir / "out2");
    std::string scene = read_file(dir / "out2" / "scenes" / "player.tscn");
    CHECK(scene.find("hframes = 4") != std::string::npos && scene.find("[node name=\"Label\"") != std::string::npos);
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// Stores record their format: older ones are upgraded on open, newer ones refused.
TEST(store_format_versions) {
  fs::path dir = temp_dir("storever");
  std::string db = to_generic(dir / "s.db");
  {
    Store s(db);
    s.exec("UPDATE meta SET value = '1' WHERE key = 'schema_version'");
  }
  {
    Store s(db);  // upgrades 1 -> current
    Stmt st(s.db(), "SELECT value FROM meta WHERE key = 'schema_version'");
    CHECK(st.step() && st.text(0) == std::to_string(kStoreVersion));
    s.exec("UPDATE meta SET value = '99' WHERE key = 'schema_version'");
  }
  CHECK(throws([&] { Store s(db); }));  // written by a newer hashstash
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(roundtrip_corpus) {
  const char* dir = std::getenv("HS_ROUNDTRIP_DIR");
  if (!dir) return;
  fs::path tmp = temp_dir("corpus");
  int files = 0, bad = 0;
  {
    Store s(to_generic(tmp / "store.db"));
    s.exec("BEGIN");
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
      if (!it->is_regular_file()) continue;
      std::string p = to_generic(it->path());
      if (!language_for_path(p)) continue;
      std::string bytes = read_file(it->path());
      ++files;
      if (!roundtrip_file(s, p, bytes)) {
        ++bad;
        if (bad < 10) std::cerr << "  round-trip mismatch: " << p << "\n";
      }
    }
    s.exec("COMMIT");
  }
  std::cerr << "  corpus: " << files << " files, " << bad << " mismatches\n";
  CHECK(bad == 0);
  fs::remove_all(tmp);
}

int main() {
  for (auto& t : tests()) {
    int before = g_failed;
    try {
      t.fn();
    } catch (std::exception& e) {
      ++g_failed;
      std::cerr << t.name << ": exception: " << e.what() << "\n";
    }
    std::cerr << (g_failed == before ? "ok   " : "FAIL ") << t.name << "\n";
  }
  std::cerr << g_passed << " checks passed, " << g_failed << " failed\n";
  return g_failed ? 1 : 0;
}
