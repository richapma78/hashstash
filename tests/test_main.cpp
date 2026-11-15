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
