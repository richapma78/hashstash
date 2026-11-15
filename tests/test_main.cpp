// Unit tests. Set HS_ROUNDTRIP_DIR to a folder of Python code to also check
// that every file in it imports and renders back byte-for-byte.
#include <cstdlib>
#include <functional>
#include <iostream>
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
#include "hub/service.h"
#include "ops/history.h"
#include "ops/locks.h"
#include "ops/publish.h"
#include "ops/sync.h"
#include "ops/query.h"
#include "ops/tests.h"
#include "ops/import_render.h"
#include "ops/repo.h"

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

// Calls the hub service directly, as the HTTP server would.
struct InProcessHub : HubTransport {
  HubService& svc;
  std::string project;
  InProcessHub(HubService& s, std::string p) : svc(s), project(std::move(p)) {}
  nlohmann::json call(const std::string& op, const nlohmann::json& req) override { return svc.handle(project, op, req); }
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
    std::cerr << "push2\n";
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
    std::cerr << "push3\n";
    push(*a, hub);
    bool stale = false;
    try {
      push(*b, hub);
    } catch (std::exception&) {
      stale = true;
    }
    CHECK(stale);

    // Hub publishes the branch to a git working copy.
    Repo* hub_repo = dynamic_cast<LocalRepoAccess*>(backend->project("demo", false).get())->local_repo();
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
