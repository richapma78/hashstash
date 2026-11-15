// hs: the hashstash client command line.
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "ops/context.h"
#include "ops/diff.h"
#include "ops/edit.h"
#include "ops/history.h"
#include "ops/import_render.h"
#include "ops/locks.h"
#include "ops/query.h"
#include "ops/repo.h"
#include "ops/tests.h"
#include "ops/verify.h"

#include "mcp/server.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

using namespace hs;

namespace {

const char* kUsage = R"(hs - hashstash, version control for AI/SI agentic coding

Project
  hs init [name]                      create a project store (.hs/) here
  hs import <path> [--prefix dir]     bring in existing code: split, index, graph
  hs render [--out dir] [--at ref]    generate source files (default out: ./out)
  hs status                           uncommitted changes, locks, open edits

Browse
  hs ls [prefix]                      list definitions
  hs view <name>                      show a definition's code
  hs find <words>                     search names, signatures, docstrings
  hs context <name>                   everything about a definition: code, notes,
                                      context history, tests, callers, changes

Edit (code lives in the store; edits happen in scratch files)
  hs edit <name>...                   lock definitions and open them for editing
  hs new <file>[:<Class>] [--after n] add a section for new code
  hs update [-m reason]               store the edits from the scratch files
  hs rm <name>                        delete a definition

Review and history
  hs diff [name] [--from ref] [--to ref] [--stat]
  hs commit -m "what changed and why" [--keep-locks]
  hs log [name] [-n N]
  hs history [-n N]                   lifecycle steps (import, update, commit, verify...)
  hs rollback @<id>                   restore a step's state (the latest stays in history)
  hs verify [--all]                   check policy and run linked tests

Tests (many per definition)
  hs tests <name>                     linked tests and suite hash
  hs tests link <name> <test>...      link test cases to a definition
  hs tests unlink <name> <test>...
  hs tests suggest <name>             tests that call it
  hs tests autolink                   link suggested tests wherever none are linked
  hs tests missing                    definitions with no tests

Notes and context
  hs note <name> <from>-<to> <text>   comment on a line region (kept beside the code)
  hs note rm <name> <id>
  hs why <name> <text> [--kind origin|design|change|note] [--source ref]

Locks
  hs lock <name>... | hs unlock <name>... [--force] | hs locks

Call graph
  hs graph callers|callees|reach|impact <name> [--depth N]
  hs graph path <from> <to>
  hs graph unused | cycles | entry

Agents
  hs mcp                              MCP server over stdio for this project

Global options: --as <agent>   act as this agent (or set HS_AGENT)
                --json         machine-readable output
Refs: work, head, head~N, @<history id>, <commit hash prefix>
)";

struct Args {
  std::vector<std::string> pos;
  std::map<std::string, std::string> opt;
  bool has(const std::string& k) const { return opt.count(k) > 0; }
  std::string get(const std::string& k, const std::string& d = "") const {
    auto it = opt.find(k);
    return it == opt.end() ? d : it->second;
  }
};

const std::vector<std::string> kValueOpts = {"--as", "--prefix", "--out", "--at", "--from", "--to",
                                             "-m", "-n", "--after", "--depth", "--kind", "--source"};

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    bool takes = std::find(kValueOpts.begin(), kValueOpts.end(), s) != kValueOpts.end();
    if (takes && i + 1 < argc) a.opt[s] = argv[++i];
    else if (starts_with(s, "--") && s.find('=') != std::string::npos)
      a.opt[s.substr(0, s.find('='))] = s.substr(s.find('=') + 1);
    else if (starts_with(s, "--") || (s.size() == 2 && s[0] == '-' && !std::isdigit((unsigned char)s[1])))
      a.opt[s] = "1";
    else a.pos.push_back(s);
  }
  return a;
}

bool g_json = false;

void out_json(const json& j) { std::cout << j.dump(2) << "\n"; }

void need(const Args& a, size_t n, const char* usage) {
  if (a.pos.size() < n) throw std::runtime_error(std::string("usage: hs ") + usage);
}

std::string join_rest(const Args& a, size_t from) {
  std::string s;
  for (size_t i = from; i < a.pos.size(); ++i) s += (i > from ? " " : "") + a.pos[i];
  return s;
}

void print_warnings(const std::vector<SizeWarning>& sw, const std::vector<TestWarning>& tw) {
  for (auto& w : sw)
    std::cout << "  warning: " << w.qname << " is " << w.lines << " lines (review limit " << w.limit
              << "): " << w.suggestion << "\n";
  for (auto& w : tw) std::cout << "  warning: " << w.qname << ": " << w.message << "\n";
}

json warnings_json(const std::vector<SizeWarning>& sw, const std::vector<TestWarning>& tw) {
  json a = json::array();
  for (auto& w : sw)
    a.push_back({{"name", w.qname}, {"lines", w.lines}, {"limit", w.limit}, {"suggestion", w.suggestion}});
  for (auto& w : tw) a.push_back({{"name", w.qname}, {"problem", w.message}});
  return a;
}

// ------------------------------------------------------------------ commands

int cmd_init(const Args& a) {
  fs::path root = fs::current_path();
  std::string name = a.pos.size() > 1 ? a.pos[1] : to_generic(root.filename());
  Repo::init(root, name);
  std::cout << "Created hashstash project '" << name << "' in " << to_generic(root / ".hs") << "\n";
  return 0;
}

int cmd_import(Repo& repo, const Args& a) {
  need(a, 2, "import <path> [--prefix dir]");
  auto r = import_path(repo, a.pos[1], a.get("--prefix"));
  int linked = autolink_tests(repo);
  auto missing = missing_tests(repo, repo.work_ns());
  if (g_json) {
    out_json({{"files", r.files}, {"other_files", r.blobs}, {"definitions", r.defs}, {"skipped", r.skipped},
              {"tests_linked", linked}, {"missing_tests", missing.size()},
              {"warnings", warnings_json(r.warnings, {})}});
    return 0;
  }
  std::cout << "Imported " << r.files << " source files (" << r.defs << " definitions) and " << r.blobs
            << " other files";
  if (r.skipped) std::cout << "; skipped " << r.skipped << " ignored paths";
  std::cout << ".\n";
  if (linked) std::cout << "Linked " << linked << " tests to the definitions they call.\n";
  size_t shown = 0;
  for (auto& w : r.warnings) {
    if (shown++ == 10) {
      std::cout << "  ... and " << r.warnings.size() - 10 << " more long definitions\n";
      break;
    }
    print_warnings({w}, {});
  }
  if (!missing.empty())
    std::cout << "  " << missing.size() << " functions/methods have no test cases (hs tests missing).\n";
  return 0;
}

int cmd_render(Repo& repo, const Args& a) {
  fs::path out = a.get("--out", "out");
  Hash ns = a.has("--at") ? resolve_ns(repo, a.get("--at")) : "";
  auto r = render_to(repo, out, ns);
  if (g_json) return out_json({{"written", r.written}, {"unchanged", r.unchanged}, {"deleted", r.deleted}}), 0;
  std::cout << "Rendered to " << to_generic(fs::absolute(out)) << ": " << r.written << " written, " << r.unchanged
            << " unchanged, " << r.deleted << " deleted.\n";
  return 0;
}

int cmd_status(Repo& repo, const Args&) {
  Store& s = repo.store();
  auto d = diff_namespaces(s, load_namespace(s, repo.head_ns_hash()), repo.work_ns());
  auto locks = list_locks(repo);
  auto scratch = scratch_files(repo);
  std::string verified = verification_status(repo, repo.work_ns_hash());
  if (g_json) {
    json l = json::array();
    for (auto& k : locks) l.push_back({{"name", k.name}, {"holder", k.holder}, {"until", format_time(k.expires)}});
    json sc = json::array();
    for (auto& p : scratch) sc.push_back(to_generic(p));
    out_json({{"branch", repo.branch()}, {"changes", diff_to_json(s, d, false)}, {"locks", l},
              {"scratch", sc}, {"verified", verified}});
    return 0;
  }
  auto head = repo.head_commit();
  std::cout << "On branch " << repo.branch() << (head ? " at #" + short_hash(*head) : " (no commits yet)") << "\n";
  if (d.empty()) std::cout << "No uncommitted changes.\n";
  else std::cout << "Uncommitted changes:\n" << diff_to_text(s, d, false);
  if (!verified.empty()) std::cout << "Working state verification: " << verified << "\n";
  if (!locks.empty()) {
    std::cout << "Locks:\n";
    for (auto& k : locks)
      std::cout << "  " << k.name << "  held by " << k.holder << " until " << format_time(k.expires) << "\n";
  }
  for (auto& p : scratch) std::cout << "Open edits in " << to_generic(p) << " (run: hs update)\n";
  return 0;
}

int cmd_ls(Repo& repo, const Args& a) {
  auto rows = list_defs(repo, a.pos.size() > 1 ? a.pos[1] : "");
  if (g_json) {
    json j = json::array();
    for (auto& r : rows) j.push_back({{"name", r.qname}, {"kind", r.kind}, {"hash", short_hash(r.hash)}});
    return out_json(j), 0;
  }
  for (auto& r : rows) {
    int depth = (int)std::count(r.qname.begin() + r.qname.find(':'), r.qname.end(), '.');
    std::string kind = r.kind;
    kind.resize(9, ' ');
    std::cout << short_hash(r.hash, 8) << "  " << kind << std::string(depth * 2, ' ') << r.qname << "\n";
  }
  return 0;
}

int cmd_view(Repo& repo, const Args& a) {
  need(a, 2, "view <name>");
  for (size_t i = 1; i < a.pos.size(); ++i) {
    IndexRow r = resolve_name(repo, a.pos[i]);
    std::string code = render_def(repo.store(), r.hash);
    if (g_json) {
      out_json({{"name", r.qname}, {"hash", r.hash}, {"kind", r.kind}, {"code", code}});
      continue;
    }
    std::cout << "# " << r.qname << "  #" << short_hash(r.hash) << "\n" << code;
    if (!ends_with(code, "\n")) std::cout << "\n";
  }
  return 0;
}

int cmd_find(Repo& repo, const Args& a) {
  need(a, 2, "find <words>");
  json res = find(repo, join_rest(a, 1));
  if (g_json) return out_json(res), 0;
  for (auto& r : res) {
    std::cout << r["name"].get<std::string>() << "\n";
    if (r.contains("signature") && !r["signature"].get<std::string>().empty())
      std::cout << "    " << r["signature"].get<std::string>() << "\n";
    if (r.contains("doc")) std::cout << "    " << r["doc"].get<std::string>().substr(0, 100) << "\n";
  }
  if (res.empty()) std::cout << "No matches.\n";
  return 0;
}

int cmd_context(Repo& repo, const Args& a) {
  need(a, 2, "context <name>");
  json j = context_of(repo, a.pos[1]);
  if (g_json) return out_json(j), 0;
  std::cout << j["name"].get<std::string>() << "  (" << j["kind"].get<std::string>() << ", #"
            << j["hash"].get<std::string>() << ")\n";
  if (j.contains("signature")) std::cout << "  " << j["signature"].get<std::string>() << "\n";
  if (j.contains("doc")) std::cout << "  " << j["doc"].get<std::string>() << "\n";
  if (j.contains("locked_by"))
    std::cout << "  locked by " << j["locked_by"]["holder"].get<std::string>() << "\n";
  std::cout << "\nCode:\n";
  auto lines = split_lines(j["code"].get<std::string>());
  std::map<int, std::vector<std::string>> marks;
  for (auto& n : j["notes"]) marks[n["lines"][0].get<int>()].push_back(n["id"].get<std::string>());
  for (size_t i = 0; i < lines.size(); ++i) {
    char num[16];
    std::snprintf(num, sizeof num, "%4zu", i + 1);
    std::cout << num << " | " << lines[i];
    if (!ends_with(lines[i], "\n")) std::cout << "\n";
  }
  if (!j["notes"].empty()) {
    std::cout << "\nNotes:\n";
    for (auto& n : j["notes"])
      std::cout << "  [" << n["id"].get<std::string>() << "] lines " << n["lines"][0] << "-" << n["lines"][1]
                << (n.contains("stale") ? " (stale)" : "") << ": " << n["text"].get<std::string>() << "\n";
  }
  if (!j["context"].empty()) {
    std::cout << "\nContext:\n";
    for (auto& c : j["context"])
      std::cout << "  " << c["time"].get<std::string>() << "  " << c["kind"].get<std::string>() << ": "
                << c["text"].get<std::string>() << "\n";
  }
  auto& t = j["tests"];
  std::cout << "\nTests (suite #" << t["suite_hash"].get<std::string>() << "):\n";
  for (auto& x : t["tests"]) std::cout << "  " << x["name"].get<std::string>() << "\n";
  if (t["tests"].empty()) std::cout << (t["needs_tests"].get<bool>() ? "  none linked (warning)\n" : "  none needed\n");
  std::cout << "\nCalled by:\n";
  for (auto& c : j["callers"])
    std::cout << "  " << c["caller"].get<std::string>() << " (line " << c["line"] << ", " << c["confidence"].get<std::string>() << ")\n";
  std::cout << "\nCalls:\n";
  for (auto& c : j["callees"]) {
    if (c["confidence"] == "external") continue;
    std::cout << "  " << c.value("callee", c["call"].get<std::string>()) << " (" << c["confidence"].get<std::string>() << ")\n";
  }
  if (!j["history"].empty()) {
    std::cout << "\nHistory:\n";
    for (auto& h : j["history"])
      std::cout << "  #" << h["commit"].get<std::string>() << " " << h["time"].get<std::string>() << " "
                << h["change"].get<std::string>() << ": " << h["message"].get<std::string>() << "\n";
  }
  return 0;
}

int cmd_edit(Repo& repo, const Args& a) {
  need(a, 2, "edit <name>...");
  std::vector<std::string> names(a.pos.begin() + 1, a.pos.end());
  auto files = open_for_edit(repo, names);
  if (g_json) {
    json f = json::array();
    for (auto& p : files) f.push_back(to_generic(p));
    return out_json({{"scratch", f}}), 0;
  }
  for (auto& p : files) std::cout << "Opened for editing in " << to_generic(p) << "\n";
  std::cout << "Locked for " << repo.config().lock_minutes << " minutes (renewed as you work). When done: hs update\n";
  return 0;
}

int cmd_new(Repo& repo, const Args& a) {
  need(a, 2, "new <file>[:<Class>] [--after name]");
  fs::path p = open_new(repo, a.pos[1], a.get("--after"));
  std::cout << "Write the new code under the @hs:new line in " << to_generic(p) << ", then: hs update\n";
  return 0;
}

int cmd_update(Repo& repo, const Args& a) {
  auto results = apply_scratch(repo, a.get("-m"));
  if (g_json) {
    json arr = json::array();
    for (auto& r : results)
      arr.push_back({{"section", r.header}, {"ok", r.outcome.ok}, {"error", r.outcome.error},
                     {"stored", r.outcome.changed}, {"removed", r.outcome.removed},
                     {"warnings", warnings_json(r.outcome.size_warnings, r.outcome.test_warnings)}});
    return out_json(arr), 0;
  }
  if (results.empty()) std::cout << "Nothing to update.\n";
  int failed = 0;
  for (auto& r : results) {
    if (!r.outcome.ok) {
      ++failed;
      std::cout << "FAILED " << r.header << "\n  " << r.outcome.error << "\n";
      continue;
    }
    for (auto& q : r.outcome.changed) std::cout << "stored  " << q << "\n";
    for (auto& q : r.outcome.removed) std::cout << "removed " << q << "\n";
    print_warnings(r.outcome.size_warnings, r.outcome.test_warnings);
  }
  if (failed) std::cout << failed << " section(s) left in the scratch file; fix them and run hs update again.\n";
  return failed ? 1 : 0;
}

int cmd_rm(Repo& repo, const Args& a) {
  need(a, 2, "rm <name>");
  Namespace ns = repo.work_ns();
  std::vector<EditOutcome> outs;
  for (size_t i = 1; i < a.pos.size(); ++i) {
    IndexRow r = resolve_name(repo, a.pos[i]);
    outs.push_back(delete_def(repo, ns, r.qname));
    if (!outs.back().ok) throw std::runtime_error(outs.back().error);
  }
  std::vector<EditOutcome*> ptrs;
  for (auto& o : outs) ptrs.push_back(&o);
  commit_edits(repo, ns, ptrs, a.get("-m"));
  for (auto& o : outs)
    for (auto& q : o.removed) std::cout << "removed " << q << "\n";
  return 0;
}

int cmd_diff(Repo& repo, const Args& a) {
  Store& s = repo.store();
  Hash from = resolve_ns(repo, a.get("--from", "head"));
  Hash to = resolve_ns(repo, a.get("--to", "work"));
  auto d = diff_namespaces(s, load_namespace(s, from), load_namespace(s, to));
  if (a.pos.size() > 1) {  // one definition
    IndexRow r = resolve_name(repo, a.pos[1]);
    std::erase_if(d.defs, [&](const DefChange& c) { return c.lineage != r.lineage; });
    d.files_added.clear();
    d.files_removed.clear();
    d.files_glue_changed.clear();
    d.tests_changed = false;
  }
  bool text = !a.has("--stat");
  if (g_json) return out_json(diff_to_json(s, d, text)), 0;
  if (d.empty()) std::cout << "No differences.\n";
  else std::cout << diff_to_text(s, d, text);
  return 0;
}

int cmd_commit(Repo& repo, const Args& a) {
  auto r = commit(repo, a.get("-m"), a.has("--keep-locks"));
  if (g_json) return out_json({{"commit", r.commit}, {"changes", diff_to_json(repo.store(), r.diff, false)}}), 0;
  std::cout << "Committed #" << short_hash(r.commit) << " on " << repo.branch() << "\n"
            << diff_to_text(repo.store(), r.diff, false);
  if (r.locks_released) std::cout << "Released " << r.locks_released << " lock(s).\n";
  if (verification_status(repo, load_commit(repo.store(), r.commit).ns) != "passed")
    std::cout << "Not verified yet: run hs verify before pushing.\n";
  return 0;
}

int cmd_log(Repo& repo, const Args& a) {
  std::string name;
  if (a.pos.size() > 1) name = resolve_name(repo, a.pos[1]).qname;
  json j = log(repo, std::stoi(a.get("-n", "20")), name);
  if (g_json) return out_json(j), 0;
  for (auto& c : j) {
    std::cout << "#" << c["commit"].get<std::string>() << "  " << c["time"].get<std::string>() << "  "
              << c["author"].get<std::string>() << "\n    " << c["message"].get<std::string>() << "\n";
    if (c.contains("change")) std::cout << "    " << c["change"].get<std::string>() << " " << c["name"].get<std::string>() << "\n";
    if (c.contains("changes"))
      for (auto& x : c["changes"]) std::cout << "    " << x.get<std::string>() << "\n";
  }
  if (j.empty()) std::cout << "No commits.\n";
  return 0;
}

int cmd_history(Repo& repo, const Args& a) {
  auto entries = history(repo, std::stoi(a.get("-n", "30")));
  Hash cur = repo.work_ns_hash();
  if (g_json) {
    json j = json::array();
    for (auto& e : entries)
      j.push_back({{"id", e.id}, {"time", format_time(e.time)}, {"step", e.step}, {"agent", e.agent},
                   {"message", e.message}, {"current", e.ns == cur}});
    return out_json(j), 0;
  }
  bool marked = false;
  for (auto& e : entries) {
    std::string step = e.step;
    step.resize(9, ' ');
    bool here = !marked && e.ns == cur;
    marked = marked || here;
    std::cout << (here ? "* " : "  ") << "@" << e.id << "  " << format_time(e.time) << "  " << step
              << e.message << "\n";
  }
  return 0;
}

int cmd_rollback(Repo& repo, const Args& a) {
  need(a, 2, "rollback @<id>");
  std::string id = a.pos[1];
  if (id[0] == '@') id = id.substr(1);
  auto e = rollback(repo, std::stoll(id));
  std::cout << "Working state restored to @" << e.id << " (" << e.step << ": " << e.message
            << ").\nThe state before the rollback is still in hs history.\n";
  return 0;
}

int cmd_verify(Repo& repo, const Args& a) {
  json r = verify(repo, a.has("--all"));
  if (g_json) return out_json(r), r["status"] == "passed" ? 0 : 1;
  for (auto& t : r["tests_run"]) std::cout << (t["status"] == "passed" ? "  pass  " : "  FAIL  ") << t["test"].get<std::string>() << "\n";
  for (auto& w : r["warnings"]) std::cout << "  warning: " << w["name"].get<std::string>() << ": " << w["problem"].get<std::string>() << "\n";
  for (auto& f : r["failures"]) std::cout << "  failure: " << f["name"].get<std::string>() << ": " << f["problem"].get<std::string>() << "\n";
  std::cout << "Verification " << r["status"].get<std::string>() << " (" << r["tests_run"].size() << " tests run).\n";
  return r["status"] == "passed" ? 0 : 1;
}

int cmd_tests(Repo& repo, const Args& a) {
  need(a, 2, "tests <name> | link | unlink | suggest | autolink | missing");
  const std::string& sub = a.pos[1];
  auto names = [&](size_t from) { return std::vector<std::string>(a.pos.begin() + from, a.pos.end()); };
  if (sub == "link" || sub == "unlink") {
    need(a, 4, "tests link|unlink <name> <test>...");
    std::string def = resolve_name(repo, a.pos[2]).qname;
    if (sub == "link") link_tests(repo, def, names(3));
    else unlink_tests(repo, def, names(3));
    std::cout << (sub == "link" ? "Linked " : "Unlinked ") << a.pos.size() - 3 << " test(s) "
              << (sub == "link" ? "to " : "from ") << def << "\n";
  } else if (sub == "suggest") {
    need(a, 3, "tests suggest <name>");
    auto s = suggest_tests(repo, resolve_name(repo, a.pos[2]).qname);
    if (g_json) return out_json(s), 0;
    for (auto& t : s) std::cout << t << "\n";
    if (s.empty()) std::cout << "No tests call it.\n";
  } else if (sub == "autolink") {
    std::cout << "Linked " << autolink_tests(repo) << " test(s).\n";
  } else if (sub == "missing") {
    auto m = missing_tests(repo, repo.work_ns());
    if (g_json) {
      json j = json::array();
      for (auto& w : m) j.push_back(w.qname);
      return out_json(j), 0;
    }
    for (auto& w : m) std::cout << w.qname << "\n";
    std::cout << m.size() << " definition(s) without test cases.\n";
  } else {
    json j = tests_info(repo, sub);
    if (g_json) return out_json(j), 0;
    std::cout << j["name"].get<std::string>() << "  #" << j["hash"].get<std::string>() << "  suite #"
              << j["suite_hash"].get<std::string>() << "\n";
    for (auto& t : j["tests"]) std::cout << "  " << t["name"].get<std::string>() << "  #" << t["hash"].get<std::string>() << "\n";
    if (j["tests"].empty()) std::cout << "  no tests linked\n";
    if (j.contains("suggested"))
      for (auto& s : j["suggested"]) std::cout << "  suggested: " << s.get<std::string>() << "\n";
  }
  return 0;
}

int cmd_note(Repo& repo, const Args& a) {
  need(a, 3, "note <name> <from>-<to> <text>  |  note rm <name> <id>");
  Namespace ns = repo.work_ns();
  if (a.pos[1] == "rm") {
    need(a, 4, "note rm <name> <id>");
    std::string q = resolve_name(repo, a.pos[2]).qname;
    if (!remove_note(repo, ns, q, a.pos[3])) throw std::runtime_error("no note " + a.pos[3] + " on " + q);
    repo.set_work_ns(ns, "note", "removed note " + a.pos[3] + " from " + q);
    std::cout << "Removed note " << a.pos[3] << "\n";
    return 0;
  }
  need(a, 4, "note <name> <from>-<to> <text>");
  std::string q = resolve_name(repo, a.pos[1]).qname;
  std::string range = a.pos[2];
  size_t dash = range.find('-');
  int from = std::stoi(range.substr(0, dash));
  int to = dash == std::string::npos ? from : std::stoi(range.substr(dash + 1));
  std::string id = add_note(repo, ns, q, from, to, join_rest(a, 3));
  repo.set_work_ns(ns, "note", "note on " + q + " lines " + range);
  std::cout << "Added note " << id << " to " << q << " lines " << from << "-" << to << "\n";
  return 0;
}

int cmd_why(Repo& repo, const Args& a) {
  need(a, 3, "why <name> <text> [--kind origin|design|change|note] [--source ref]");
  Namespace ns = repo.work_ns();
  std::string q = resolve_name(repo, a.pos[1]).qname;
  std::string kind = a.get("--kind", "note");
  add_context(repo, ns, q, kind, join_rest(a, 2), a.get("--source"));
  repo.set_work_ns(ns, "context", kind + " context for " + q);
  std::cout << "Recorded " << kind << " context for " << q << "\n";
  return 0;
}

int cmd_lock(Repo& repo, const Args& a, bool lock) {
  if (lock) need(a, 2, "lock <name>...");
  else need(a, 2, "unlock <name>... [--force]");
  for (size_t i = 1; i < a.pos.size(); ++i) {
    IndexRow r = resolve_name(repo, a.pos[i]);
    if (lock) {
      auto l = acquire_lock(repo, r.lineage, r.qname);
      std::cout << "Locked " << r.qname << " until " << format_time(l.expires) << "\n";
    } else {
      bool ok = release_lock(repo, r.lineage, a.has("--force"));
      std::cout << (ok ? "Unlocked " : "Was not locked: ") << r.qname << "\n";
    }
  }
  return 0;
}

int cmd_locks(Repo& repo, const Args&) {
  auto locks = list_locks(repo);
  if (g_json) {
    json j = json::array();
    for (auto& l : locks) j.push_back({{"name", l.name}, {"holder", l.holder}, {"until", format_time(l.expires)}});
    return out_json(j), 0;
  }
  for (auto& l : locks) std::cout << l.name << "  " << l.holder << "  until " << format_time(l.expires) << "\n";
  if (locks.empty()) std::cout << "No locks.\n";
  return 0;
}

int cmd_graph(Repo& repo, const Args& a) {
  need(a, 2, "graph callers|callees|reach|impact|path|unused|cycles|entry ...");
  const std::string& q = a.pos[1];
  int depth = std::stoi(a.get("--depth", "3"));
  json j;
  if (q == "unused") j = unused(repo);
  else if (q == "cycles") j = cycles(repo);
  else if (q == "entry") j = entry_points(repo);
  else if (q == "path") {
    need(a, 4, "graph path <from> <to>");
    j = call_path(repo, resolve_name(repo, a.pos[2]).qname, resolve_name(repo, a.pos[3]).qname);
  } else {
    need(a, 3, "graph callers|callees|reach|impact <name>");
    std::string n = resolve_name(repo, a.pos[2]).qname;
    if (q == "callers") j = callers(repo, n);
    else if (q == "callees") j = callees(repo, n);
    else if (q == "reach") j = reach(repo, n, depth, false);
    else if (q == "impact") j = reach(repo, n, depth, true);
    else throw std::runtime_error("unknown graph query: " + q);
  }
  if (g_json) return out_json(j), 0;
  if (q == "callers")
    for (auto& e : j) std::cout << e["caller"].get<std::string>() << "  line " << e["line"] << "  " << e["confidence"].get<std::string>() << "\n";
  else if (q == "callees")
    for (auto& e : j) std::cout << "line " << e["line"] << "  " << e["call"].get<std::string>() << " -> "
                                << e.value("callee", std::string("?")) << "  " << e["confidence"].get<std::string>() << "\n";
  else if (q == "reach" || q == "impact") {
    int lvl = 1;
    for (auto& level : j["levels"]) {
      for (auto& n : level) std::cout << std::string(lvl * 2, ' ') << n["name"].get<std::string>() << "\n";
      ++lvl;
    }
    std::cout << j["count"] << " definitions\n";
  } else if (q == "path") {
    if (j["path"].is_null()) std::cout << "No call path found.\n";
    else for (auto& n : j["path"]) std::cout << "  " << n.get<std::string>() << "\n";
  } else if (q == "cycles") {
    for (auto& c : j) {
      for (auto& n : c) std::cout << n.get<std::string>() << "  ";
      std::cout << "\n";
    }
  } else {
    for (auto& e : j) std::cout << e["name"].get<std::string>() << "\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
#endif
  Args a = parse(argc, argv);
  g_json = a.has("--json");
  if (a.pos.empty() || a.pos[0] == "help" || a.has("--help") || a.has("-h")) {
    std::cout << kUsage;
    return 0;
  }
  const std::string cmd = a.pos[0];
  try {
    if (cmd == "version" || cmd == "--version") {
      std::cout << "hs " << HS_VERSION << "\n";
      return 0;
    }
    if (cmd == "init") return cmd_init(a);
    auto repo = Repo::open(fs::current_path());
    if (a.has("--as")) repo->set_agent(a.get("--as"));
    if (cmd == "import") return cmd_import(*repo, a);
    if (cmd == "render") return cmd_render(*repo, a);
    if (cmd == "status") return cmd_status(*repo, a);
    if (cmd == "ls") return cmd_ls(*repo, a);
    if (cmd == "view") return cmd_view(*repo, a);
    if (cmd == "find") return cmd_find(*repo, a);
    if (cmd == "context") return cmd_context(*repo, a);
    if (cmd == "edit") return cmd_edit(*repo, a);
    if (cmd == "new") return cmd_new(*repo, a);
    if (cmd == "update") return cmd_update(*repo, a);
    if (cmd == "rm") return cmd_rm(*repo, a);
    if (cmd == "diff") return cmd_diff(*repo, a);
    if (cmd == "commit") return cmd_commit(*repo, a);
    if (cmd == "log") return cmd_log(*repo, a);
    if (cmd == "history") return cmd_history(*repo, a);
    if (cmd == "rollback") return cmd_rollback(*repo, a);
    if (cmd == "verify") return cmd_verify(*repo, a);
    if (cmd == "tests") return cmd_tests(*repo, a);
    if (cmd == "note") return cmd_note(*repo, a);
    if (cmd == "why") return cmd_why(*repo, a);
    if (cmd == "lock") return cmd_lock(*repo, a, true);
    if (cmd == "unlock") return cmd_lock(*repo, a, false);
    if (cmd == "locks") return cmd_locks(*repo, a);
    if (cmd == "graph") return cmd_graph(*repo, a);
    if (cmd == "mcp") {
#ifdef _WIN32
      _setmode(_fileno(stdout), _O_BINARY);
      _setmode(_fileno(stdin), _O_BINARY);
#endif
      std::ios::sync_with_stdio(false);
      McpServer(*repo).serve(std::cin, std::cout);
      return 0;
    }
    std::cerr << "hs: unknown command '" << cmd << "' (see: hs help)\n";
    return 2;
  } catch (std::exception& e) {
    std::cerr << "hs: " << e.what() << "\n";
    return 1;
  }
}
