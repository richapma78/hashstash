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
#include "ops/publish.h"
#include "ops/sync.h"
#include "ops/query.h"
#include "ops/repo.h"
#include "ops/review.h"
#include "ops/tests.h"
#include "ops/verify.h"
#include "ops/work.h"

#include "mcp/server.h"
#include "web/ui.h"

#include <httplib.h>

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
  hs commit -m "what changed and why" [--keep-locks] [--resolves ids]
  hs log [name] [-n N]
  hs history [-n N]                   lifecycle steps (import, update, commit, verify...)
  hs rollback @<id>                   restore a step's state (the latest stays in history)
  hs verify [--all] [-v]              check policy and run linked tests (one line per function)
  hs show [commit]                    a commit: per-function line diffs, test summary, reviews
  hs review [commit] approve|changes|comment [-m text]
                                      record your review (a person's, or an AI's under an agent)
  hs reviews [-n N]                   review flags of recent commits
  hs ui [--port 7421] [--no-open]     review pages in the browser for this project

Tests (many per definition)
  hs tests <name>                     linked tests and suite hash
  hs tests link <name> <test>...      link test cases to a definition
  hs tests unlink <name> <test>...
  hs tests suggest <name>             tests that call it
  hs tests autolink                   link suggested tests wherever none are linked
  hs tests missing                    definitions with no tests
  hs tests summary [prefix] [--problems]  results per function: "18/20 passed, 2 failed"

Notes and context
  hs note <name> <from>-<to> <text>   comment on a line region (kept beside the code)
  hs note rm <name> <id>
  hs why <name> <text> [--kind origin|design|change|note] [--source ref]

Locks and work items (many agents on one codebase)
  hs work start <id> --title "..." [--kind feature|bug|chore] [--priority N] [--link url]
                                      what you are working on: locks and requests carry it
  hs work [status] | list [--all] | done [id] | abandon [id]
  hs lock <name>... | hs unlock <name>... [--force] | hs locks
  hs request <name> --need "..." [--test-file f --test-code file|-]
                                      someone holds it: ask them to make your change before they
                                      commit it (your tests get linked to it), instead of waiting
  hs request <name> --lock            queue for the lock instead (yours when released)
  hs request accept|decline|cancel <id> [--reason "..."]
  hs requests [--all] [--closed]
  hs commit ... --resolves 3,4        commit that makes requested changes (required while
                                      requests are open on what you commit; [work] requests)
  hs inbox [--wait S] [--peek] [--follow]
                                      your events: new requests, answers, locks handed to you,
                                      requests done (exit 3 when there is nothing)
  hs subscribe <url> | unsubscribe <url>   have the hub post your events to a webhook

Call graph
  hs graph callers|callees|reach|impact <name> [--depth N]
  hs graph path <from> <to>
  hs graph unused | cycles | entry

Hub (hshub)
  hs remote add <name> <url>          e.g. hs remote add origin http://localhost:7420/myproject
  hs clone <url> [dir]                copy a hub project
  hs push [remote] [branch]           send verified commits (and your locks) to the hub
  hs pull [remote] [branch]           fast-forward from the hub (and receive its locks)
  hs publish [branch]                 render into the [publish] git working copy, commit, push
  With remote "origin" set, locks are taken on the hub too (HS_OFFLINE=1 to skip).

Agents
  hs mcp [--channel]                  MCP server over stdio for this project; --channel also
                                      pushes your events into the session (Claude Code channels)

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
                                             "-m", "-n", "--after", "--depth", "--kind", "--source", "--port",
                                             "--title", "--priority", "--link", "--need", "--test-file",
                                             "--test-code", "--reason", "--resolves", "--wait"};

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
    kind.resize(10, ' ');
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
    d.module_changes.clear();
    d.tests_changed = false;
  }
  bool text = !a.has("--stat");
  if (g_json) return out_json(diff_to_json(s, d, text)), 0;
  if (d.empty()) std::cout << "No differences.\n";
  else std::cout << diff_to_text(s, d, text);
  return 0;
}

long long request_id(const std::string& s);

int cmd_commit(Repo& repo, const Args& a) {
  std::vector<long long> resolves;
  {
    std::string list = a.get("--resolves") + ",", id;
    for (char c : list)
      if (c != ',') {
        id += c;
      } else {
        if (!trim(id).empty()) resolves.push_back(request_id(trim(id)));
        id.clear();
      }
  }
  auto wc = commit_work(repo, a.get("-m"), resolves, a.has("--keep-locks"));
  auto& r = wc.result;
  if (g_json) {
    json res = json::array(), open = json::array();
    for (auto& q : wc.resolved) res.push_back(q.to_json());
    for (auto& q : wc.left_open) open.push_back(q.to_json());
    return out_json({{"commit", r.commit}, {"changes", diff_to_json(repo.store(), r.diff, false)},
                     {"resolved", res}, {"left_open", open}, {"requests_done", wc.landed}}), 0;
  }
  std::cout << "Committed #" << short_hash(r.commit) << " on " << repo.branch() << "\n";
  for (auto& q : wc.resolved)
    std::cout << "Resolves R-" << q.id << " for " << q.requester << (wc.landed ? " (done)" : " (done when pushed)") << "\n";
  for (auto& q : wc.left_open) std::cout << "Still open: " << describe_request(q) << "\n";
  if (!wc.warning.empty()) std::cout << "Note: " << wc.warning << "\n";
  auto lines = split_lines(diff_to_text(repo.store(), r.diff, false));
  for (size_t i = 0; i < lines.size() && i < 25; ++i) std::cout << lines[i];
  if (lines.size() > 25) std::cout << "  ... and " << lines.size() - 25 << " more (hs log --json for all)\n";
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
    if (c.contains("changes")) {
      size_t shown = 0;
      for (auto& x : c["changes"])
        if (shown++ < 10) std::cout << "    " << x.get<std::string>() << "\n";
      if (shown > 10) std::cout << "    ... and " << shown - 10 << " more\n";
    }
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
  if (!r["impacted"].empty()) {
    std::cout << "  also testing callers:";
    for (auto& n : r["impacted"]) std::cout << " " << n.get<std::string>();
    std::cout << "\n";
  }
  // One line per function ("3/4 passed, 1 failed"); individual tests only when they fail.
  for (auto& f : r["functions"]) {
    std::string label = f["summary"];
    label.resize(std::max<size_t>(label.size() + 2, 24), ' ');
    std::cout << "  " << label << f["name"].get<std::string>() << "\n";
  }
  for (auto& t : r["tests_run"])
    if (t["status"] != "passed" || a.has("-v")) std::cout << (t["status"] == "passed" ? "  pass  " : "  FAIL  ") << t["test"].get<std::string>() << "\n";
  for (auto& w : r["warnings"]) std::cout << "  warning: " << w["name"].get<std::string>() << ": " << w["problem"].get<std::string>() << "\n";
  for (auto& f : r["failures"]) std::cout << "  failure: " << f["name"].get<std::string>() << ": " << f["problem"].get<std::string>() << "\n";
  size_t n = r["tests_run"].size();
  std::cout << "Verification " << r["status"].get<std::string>() << " (" << n << (n == 1 ? " test" : " tests") << " run).\n";
  return r["status"] == "passed" ? 0 : 1;
}

int cmd_tests_summary(Repo& repo, const Args& a);

std::string first_line_of(const json& msg) {
  std::string m = msg.get<std::string>();
  return m.substr(0, m.find('\n'));
}

int cmd_tests(Repo& repo, const Args& a) {
  need(a, 2, "tests <name> | link | unlink | suggest | autolink | missing | summary");
  if (a.pos[1] == "summary") return cmd_tests_summary(repo, a);
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
    if (j.contains("results")) std::cout << "  results on this code: " << j["results"]["summary"].get<std::string>() << "\n";
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
    for (auto& l : locks)
      j.push_back({{"name", l.name}, {"holder", l.holder}, {"work", l.work}, {"until", format_time(l.expires)}});
    return out_json(j), 0;
  }
  for (auto& l : locks)
    std::cout << l.name << "  " << l.holder << (l.work.empty() ? "" : " for " + l.work) << "  until "
              << format_time(l.expires) << "\n";
  if (locks.empty()) std::cout << "No locks.\n";
  return 0;
}


// ---- work items, requests, inbox ------------------------------------------------

long long request_id(const std::string& s) {
  std::string t = s;
  if (t.size() > 2 && (t[0] == 'R' || t[0] == 'r') && t[1] == '-') t = t.substr(2);
  try {
    return std::stoll(t);
  } catch (...) {
    throw std::runtime_error("not a request id: " + s + " (e.g. R-3 or 3)");
  }
}

void print_work(const WorkItem& w) {
  std::cout << w.id << "  [" << w.kind << ", " << w.status << ", priority " << w.priority << "]  " << w.title
            << "  (owner " << w.owner << ")" << (w.link.empty() ? "" : "  " + w.link) << "\n";
}

int cmd_work(Repo& repo, const Args& a) {
  std::string sub = a.pos.size() > 1 ? a.pos[1] : "";
  if (sub.empty() || sub == "status") {
    json st = work_status(repo);
    if (g_json) return out_json(st), 0;
    if (st["work"].is_null()) std::cout << "No current work item (hs work start <id> --title \"...\").\n";
    else print_work(WorkItem::from_json(st["work"]));
    for (auto& l : st["locks"]) std::cout << "  lock  " << l["name"].get<std::string>() << " until " << l["until"].get<std::string>() << "\n";
    for (auto& r : st["requests_for_you"]) std::cout << "  for you   " << r["summary"].get<std::string>() << "\n";
    for (auto& r : st["your_requests"]) std::cout << "  yours     " << r["summary"].get<std::string>() << "\n";
    if (st["inbox"].get<size_t>()) std::cout << "  " << st["inbox"].get<size_t>() << " new event(s): hs inbox\n";
    return 0;
  }
  if (sub == "start") {
    need(a, 3, "work start <id> [--title \"...\"] [--kind feature|bug|chore] [--priority N] [--link url]");
    WorkItem w;
    w.id = a.pos[2];
    w.kind = a.get("--kind");
    w.title = a.get("--title");
    w.link = a.get("--link");
    if (a.has("--priority")) w.priority = std::stoi(a.get("--priority"));
    WorkItem out = start_work(repo, w);
    if (g_json) return out_json(out.to_json()), 0;
    std::cout << "Working on ";
    print_work(out);
    return 0;
  }
  if (sub == "list") {
    auto ws = coordination(repo)->works(a.has("--all"));
    if (g_json) {
      json j = json::array();
      for (auto& w : ws) j.push_back(w.to_json());
      return out_json(j), 0;
    }
    for (auto& w : ws) print_work(w);
    if (ws.empty()) std::cout << "No open work items.\n";
    return 0;
  }
  if (sub == "done" || sub == "abandon") {
    WorkItem w = finish_work(repo, a.pos.size() > 2 ? a.pos[2] : "", sub == "done" ? "done" : "abandoned");
    if (g_json) return out_json(w.to_json()), 0;
    print_work(w);
    return 0;
  }
  throw std::runtime_error("usage: hs work [status] | start <id> ... | list [--all] | done [id] | abandon [id]");
}

int cmd_request(Repo& repo, const Args& a) {
  need(a, 2, "request <name> --need \"...\" [--test-file f --test-code file] | request accept|decline|cancel <id>");
  std::string sub = a.pos[1];
  if ((sub == "accept" || sub == "decline" || sub == "cancel") && a.pos.size() > 2) {
    long long id = request_id(a.pos[2]);
    Request r = sub == "accept" ? accept_request(repo, id)
                : sub == "decline" ? decline_request(repo, id, a.get("--reason", a.get("-m")))
                                   : cancel_request(repo, id);
    if (g_json) return out_json(r.to_json()), 0;
    std::cout << describe_request(r) << "\n";
    if (sub == "accept")
      std::cout << "Make the change, verify, then: hs commit -m \"...\" --resolves " << r.id << "\n";
    return 0;
  }
  std::string code;
  if (a.has("--test-code")) code = a.get("--test-code") == "-" ? std::string(std::istreambuf_iterator<char>(std::cin), {})
                                                                 : read_file(from_utf8(a.get("--test-code")));
  Request r = make_request(repo, sub, a.has("--lock") ? "lock" : "change", a.get("--need", a.get("-m")),
                           a.get("--test-file"), code);
  if (g_json) return out_json(r.to_json()), 0;
  std::cout << describe_request(r) << "\n";
  std::cout << (r.kind == "lock" ? "You get the lock when " + r.holder + " releases it."
                                 : r.holder + " is told and must deal with it before committing " + r.name + ".")
            << " Watch for the answer: hs inbox --wait 300\n";
  return 0;
}

int cmd_requests(Repo& repo, const Args& a) {
  RequestFilter f;
  if (!a.has("--all")) f.agent = repo.agent();
  f.open_only = !a.has("--closed");
  auto rs = coordination(repo)->requests(f);
  if (g_json) {
    json j = json::array();
    for (auto& r : rs) j.push_back(r.to_json());
    return out_json(j), 0;
  }
  for (auto& r : rs) std::cout << describe_request(r) << "\n";
  if (rs.empty()) std::cout << "No requests.\n";
  return 0;
}

// Exit code 0 when there were events, 3 when there were none (for scripts
// and background waits that should wake an agent only when something came).
int cmd_inbox(Repo& repo, const Args& a) {
  int wait = std::stoi(a.get("--wait", "0"));
  bool follow = a.has("--follow");
  for (;;) {
    auto events = inbox(repo, !a.has("--peek"), follow ? 25 : wait);
    if (g_json) {
      json j = json::array();
      for (auto& e : events) {
        json x = e.to_json();
        x["text"] = describe_event(e);
        j.push_back(x);
      }
      std::cout << j.dump(follow ? -1 : 2) << "\n";
    } else {
      for (auto& e : events) std::cout << format_time(e.time) << "  " << describe_event(e) << "\n";
    }
    std::cout.flush();
    if (!follow) {
      if (events.empty() && !g_json) std::cout << "Nothing new.\n";
      return events.empty() ? 3 : 0;
    }
  }
}

int cmd_subscribe(Repo& repo, const Args& a, bool on) {
  auto c = coordination(repo);
  if (a.pos.size() < 2) {
    for (auto& s : c->subscriptions()) std::cout << s.agent << "  " << s.url << "  (after event " << s.after << ")\n";
    return 0;
  }
  if (on) {
    auto s = c->subscribe(repo.agent(), a.pos[1]);
    std::cout << "Events for " << s.agent << " will be posted to " << s.url << "\n";
  } else {
    std::cout << (c->unsubscribe(repo.agent(), a.pos[1]) ? "Unsubscribed.\n" : "No such subscription.\n");
  }
  return 0;
}

int cmd_remote(Repo& repo, const Args& a) {
  if (a.pos.size() >= 4 && a.pos[1] == "add") {
    set_remote(repo, a.pos[2], a.pos[3]);
    std::cout << "Remote " << a.pos[2] << " -> " << a.pos[3] << "\n";
    return 0;
  }
  for (auto& [k, v] : repo.store().refs_with_prefix("remote/"))
    if (std::count(k.begin(), k.end(), '/') == 1) std::cout << k.substr(7) << "  " << v << "\n";
  return 0;
}

int cmd_push(Repo& repo, const Args& a) {
  std::string remote = a.pos.size() > 1 ? a.pos[1] : "origin";
  auto hub = open_remote(repo, remote);
  json r = push(repo, *hub, remote, a.pos.size() > 2 ? a.pos[2] : "");
  if (g_json) return out_json(r), 0;
  if (r.contains("note")) std::cout << r["note"].get<std::string>() << "\n";
  else
    std::cout << "Pushed " << r["branch"].get<std::string>() << " #" << short_hash(r["head"]) << " to " << remote
              << " (" << r["uploaded"] << " objects uploaded).\n";
  if (r.contains("commits") && r["commits"].get<int>() > 0) {
    int n = r["commits"], human = r["human_reviewed"], ai = r["ai_reviewed_only"], none = r["unreviewed"];
    std::cout << "Reviews: " << human << " of " << n << " commit(s) reviewed by a person";
    if (ai) std::cout << ", " << ai << " by AI only";
    if (none) std::cout << ", " << none << " unreviewed";
    std::cout << " (approval is optional; hs ui to review).\n";
  }
  return 0;
}

int cmd_pull(Repo& repo, const Args& a) {
  std::string remote = a.pos.size() > 1 ? a.pos[1] : "origin";
  auto hub = open_remote(repo, remote);
  json r = pull(repo, *hub, remote, a.pos.size() > 2 ? a.pos[2] : "");
  if (g_json) return out_json(r), 0;
  std::cout << (r.contains("result") ? r["result"].get<std::string>() : r.value("note", std::string("done")));
  if (r.contains("fetched")) std::cout << " (" << r["fetched"] << " objects fetched, " << r["locks"] << " hub locks)";
  std::cout << "\n";
  return 0;
}

int cmd_clone(const Args& a) {
  need(a, 2, "clone <http://host:port/project> [dir]");
  std::string url = a.pos[1];
  std::string name = url.substr(url.find_last_of('/') + 1);
  fs::path dir = a.pos.size() > 2 ? from_utf8(a.pos[2]) : from_utf8(name);
  fs::create_directories(dir);
  auto repo = Repo::init(dir, name);
  if (a.has("--as")) repo->set_agent(a.get("--as"));
  set_remote(*repo, "origin", url);
  auto hub = open_remote(*repo, "origin");
  json r = pull(*repo, *hub, "origin", "main");
  std::cout << "Cloned " << url << " into " << to_generic(fs::absolute(dir)) << ": "
            << r.value("result", r.value("note", std::string("empty"))) << "\n";
  return 0;
}

int cmd_publish(Repo& repo, const Args& a) {
  json r = publish(repo, a.pos.size() > 1 ? a.pos[1] : "");
  if (g_json) return out_json(r), 0;
  std::cout << "Published #" << r["commit"].get<std::string>() << " to " << r["dir"].get<std::string>() << ": "
            << r["written"] << " files written, " << r["deleted"] << " deleted"
            << (r.contains("git_commit") ? ", git commit " + r["git_commit"].get<std::string>() : ", no changes")
            << (r.contains("pushed") ? ", pushed to " + r["pushed"].get<std::string>() : "") << ".\n";
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

void print_show(const json& j) {
  std::cout << "#" << j["short"].get<std::string>() << "  " << j["when"].get<std::string>() << "  "
            << j["author"].get<std::string>() << "\n    " << j["message"].get<std::string>() << "\n";
  std::cout << "    tests: " << j["verification"].get<std::string>() << "   review: " << j["review"]["label"].get<std::string>()
            << "\n\n";
  auto& d = j["diff"];
  for (auto& f : d["files_added"]) std::cout << "  new file      " << f.get<std::string>() << "\n";
  for (auto& f : d["files_removed"]) std::cout << "  deleted file  " << f.get<std::string>() << "\n";
  auto body = [](const json& c) {
    std::string t = c.contains("diff") ? c["diff"].get<std::string>() : "";
    char mark = 0;
    if (c.contains("text")) t = c["text"], mark = '+';
    if (c.contains("removed_text")) t = c["removed_text"], mark = '-';
    for (auto& l : split_lines(t)) std::cout << "      " << (mark ? std::string(1, mark) : "") << l;
    if (!t.empty() && t.back() != '\n') std::cout << "\n";
    if (c.contains("omitted_lines")) std::cout << "      (" << c["omitted_lines"] << " lines not shown)\n";
  };
  for (auto& c : d["definitions"]) {
    std::string name = c.contains("new") ? c["new"].get<std::string>() : c["old"].get<std::string>();
    std::string label = c["change"];
    label.resize(std::max<size_t>(label.size(), 14), ' ');
    std::cout << "  " << label << name;
    if (j["tests"].contains(name)) std::cout << "   [" << j["tests"][name]["summary"].get<std::string>() << "]";
    std::cout << "\n";
    body(c);
  }
  if (d.contains("module_code"))
    for (auto& m : d["module_code"]) {
      std::cout << "  module code   " << m["file"].get<std::string>() << " (" << m["change"].get<std::string>() << ")\n";
      body(m);
    }
  if (d.contains("truncated")) std::cout << "  " << d["truncated"].get<std::string>() << "\n";
  if (!j["reviews"].empty()) {
    std::cout << "\nReviews:\n";
    for (auto& r : j["reviews"]) {
      std::cout << "  " << (r["kind"] == "human" ? "person " : "AI     ") << r["reviewer"].get<std::string>() << ": "
                << r["verdict"].get<std::string>();
      if (!r["summary"].get<std::string>().empty()) std::cout << " - " << r["summary"].get<std::string>();
      std::cout << "\n";
    }
  }
}

int cmd_show(Repo& repo, const Args& a) {
  json j = show_commit(repo, resolve_commit(repo, a.pos.size() > 1 ? a.pos[1] : "head"), 0);
  if (g_json) return out_json(j), 0;
  print_show(j);
  return 0;
}

int cmd_review(Repo& repo, const Args& a) {
  need(a, 2, "review [commit] approve|changes|comment [-m text]");
  std::string commit = "head", verdict;
  if (a.pos.size() > 2) commit = a.pos[1], verdict = a.pos[2];
  else verdict = a.pos[1];
  std::string kind = reviewer_kind_from_env();
  Review r = add_review(repo, commit, verdict, a.get("-m"), kind);
  std::string note = share_reviews(repo);
  auto st = review_status(repo.store(), r.commit);
  if (g_json) return out_json({{"recorded", review_to_json(r)}, {"flag", st.to_json()}, {"note", note}}), 0;
  std::cout << "Recorded " << (kind == "human" ? "your" : "an AI") << " review of #" << short_hash(r.commit) << ": "
            << r.verdict << ". Commit is now " << st.label() << ".\n";
  if (kind == "ai") std::cout << "(Running under an AI agent, so it counts as an AI review; HS_REVIEWER=human overrides.)\n";
  if (!note.empty()) std::cout << note << "\n";
  return 0;
}

int cmd_reviews(Repo& repo, const Args& a) {
  auto commits = branch_commits(repo, repo.branch(), std::stoi(a.get("-n", "20")));
  json out = json::array();
  for (auto& h : commits) out.push_back(commit_row(repo, h));
  if (g_json) return out_json(out), 0;
  for (auto& r : out) {
    std::string label = r["review"]["label"];
    label.resize(std::max<size_t>(label.size(), 26), ' ');
    std::cout << "#" << r["short"].get<std::string>() << "  " << label << first_line_of(r["message"]) << "\n";
  }
  if (out.empty()) std::cout << "No commits.\n";
  return 0;
}

int cmd_ui(Repo& repo, const Args& a) {
  int port = std::stoi(a.get("--port", "7421"));
  std::string name = repo.config().project.empty() ? "project" : repo.config().project;
  std::mutex mu;
  httplib::Server srv;
  UiSite site;
  site.where = "local";
  site.projects = [&] { return std::vector<std::string>{name}; };
  site.open = [&](const std::string& p) -> Repo* { return p == name ? &repo : nullptr; };
  site.mu = &mu;
  mount_ui(srv, site);
  srv.Get("/", [](const httplib::Request&, httplib::Response& res) { res.set_redirect("/ui"); });
  std::string url = "http://127.0.0.1:" + std::to_string(port) + "/ui/p/" + name;
  std::cout << "Review pages for " << name << ": " << url << "\n(Ctrl+C to stop)" << std::endl;
#ifdef _WIN32
  if (!a.has("--no-open")) ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
  if (!srv.listen("127.0.0.1", port)) throw std::runtime_error("cannot listen on port " + std::to_string(port) + " (--port to pick another)");
  return 0;
}

int cmd_tests_summary(Repo& repo, const Args& a) {
  std::string prefix = a.pos.size() > 2 ? a.pos[2] : "";
  bool problems = a.has("--problems");
  std::vector<std::pair<int, TestSummary>> rows;
  int funcs = 0, total = 0, passed = 0, failed = 0;
  for (auto& [q, t] : test_summaries(repo, repo.work_ns())) {
    if (!prefix.empty() && !starts_with(q, prefix)) continue;
    if (t.total == 0 && !t.needs_tests) continue;
    ++funcs, total += t.total, passed += t.passed, failed += t.failed;
    if (problems && t.total > 0 && t.passed == t.total) continue;
    rows.push_back({t.failed ? 0 : t.total == 0 ? 1 : t.not_run ? 2 : 3, t});
  }
  std::stable_sort(rows.begin(), rows.end(), [](auto& x, auto& y) { return x.first < y.first; });
  if (g_json) {
    json arr = json::array();
    for (auto& [k, t] : rows) {
      json j = t.to_json();
      j["name"] = t.qname;
      arr.push_back(j);
    }
    return out_json({{"functions", funcs}, {"tests", total}, {"passed", passed}, {"failed", failed}, {"rows", arr}}), 0;
  }
  for (auto& [k, t] : rows) {
    std::string label = t.label();
    label.resize(std::max<size_t>(label.size(), 28), ' ');
    std::cout << "  " << label << t.qname << "\n";
  }
  std::cout << funcs << " functions, " << passed << "/" << total << " tests passed";
  if (failed) std::cout << ", " << failed << " failed";
  std::cout << ".\n";
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
    if (cmd == "clone") return cmd_clone(a);
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
    if (cmd == "work") return cmd_work(*repo, a);
    if (cmd == "request") return cmd_request(*repo, a);
    if (cmd == "requests") return cmd_requests(*repo, a);
    if (cmd == "inbox") return cmd_inbox(*repo, a);
    if (cmd == "subscribe" || cmd == "subscriptions") return cmd_subscribe(*repo, a, true);
    if (cmd == "unsubscribe") return cmd_subscribe(*repo, a, false);
    if (cmd == "lock") return cmd_lock(*repo, a, true);
    if (cmd == "unlock") return cmd_lock(*repo, a, false);
    if (cmd == "locks") return cmd_locks(*repo, a);
    if (cmd == "graph") return cmd_graph(*repo, a);
    if (cmd == "show") return cmd_show(*repo, a);
    if (cmd == "review") return cmd_review(*repo, a);
    if (cmd == "reviews") return cmd_reviews(*repo, a);
    if (cmd == "ui") return cmd_ui(*repo, a);
    if (cmd == "remote") return cmd_remote(*repo, a);
    if (cmd == "push") return cmd_push(*repo, a);
    if (cmd == "pull") return cmd_pull(*repo, a);
    if (cmd == "publish") return cmd_publish(*repo, a);
    if (cmd == "mcp") {
#ifdef _WIN32
      _setmode(_fileno(stdout), _O_BINARY);
      _setmode(_fileno(stdin), _O_BINARY);
#endif
      std::ios::sync_with_stdio(false);
      McpServer server(*repo);
      server.enable_channel(a.has("--channel"));
      server.serve(std::cin, std::cout);
      return 0;
    }
    std::cerr << "hs: unknown command '" << cmd << "' (see: hs help)\n";
    return 2;
  } catch (std::exception& e) {
    std::cerr << "hs: " << e.what() << "\n";
    return 1;
  }
}
