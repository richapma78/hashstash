#include "mcp/server.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "ops/context.h"
#include "ops/diff.h"
#include "ops/edit.h"
#include "ops/history.h"
#include "ops/import_render.h"
#include "ops/locks.h"
#include "ops/publish.h"
#include "ops/sync.h"
#include "ops/query.h"
#include "ops/review.h"
#include "ops/tests.h"
#include "ops/verify.h"
#include "ops/work.h"

namespace hs {

using json = nlohmann::json;

static const char* kInstructions =
    "hashstash is a version control system for AI/SI agentic coding. Code is stored per "
    "definition (function, method, class), not per file, and normal source files are generated "
    "with hs_render. Workflow: hs_find / hs_context to understand a definition (code, notes, "
    "context history, tests, callers, callees, change log); hs_lock it; hs_edit_def with the new "
    "code and a reason; add or link test cases (every function and method needs its own tests); "
    "hs_verify; hs_commit with a description of what changed, check the diff it returns, then "
    "hs_review your commit. Many agents share this code: hs_work start the feature or bug you are "
    "on first (locks and requests show it). If a definition is locked, don't wait: hs_request the "
    "change you need (the holder makes it before committing) or queue for the lock. hs_commit refuses "
    "while others' requests are open on what you changed: hs_request_respond accept (adds their "
    "tests), make the change, hs_verify, then hs_commit with resolves. hs_inbox tells you when "
    "your requests are answered or done. Keep definitions small: responses "
    "warn when one grows past the review limit, so split it into helpers. Names can be "
    "'file.py:Class.method', 'module.Class.method', or a unique short name.";

namespace {

json prop(const char* type, const char* desc) { return {{"type", type}, {"description", desc}}; }

json tool(const char* name, const char* desc, json props = json::object(),
          std::vector<std::string> required = {}) {
  return {{"name", name},
          {"description", desc},
          {"inputSchema", {{"type", "object"}, {"properties", props}, {"required", required}}}};
}

json str_array(const char* desc) { return {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", desc}}; }

json size_and_test_warnings(const EditOutcome& o) {
  json w = json::array();
  for (auto& s : o.size_warnings)
    w.push_back({{"name", s.qname},
                 {"problem", "too long for review: " + std::to_string(s.lines) + " lines (limit " +
                                 std::to_string(s.limit) + ")"},
                 {"suggestion", s.suggestion}});
  for (auto& t : o.test_warnings)
    w.push_back({{"name", t.qname}, {"problem", "no test cases linked"},
                 {"suggestion", "write tests for it and link them with hs_link_tests"}});
  return w;
}

json outcome_json(const EditOutcome& o) {
  if (!o.ok) throw std::runtime_error(o.error);
  return {{"stored", o.changed}, {"removed", o.removed}, {"warnings", size_and_test_warnings(o)}};
}

std::string need_str(const json& a, const char* key) {
  if (!a.contains(key) || !a[key].is_string() || a[key].get<std::string>().empty())
    throw std::runtime_error(std::string("missing argument: ") + key);
  return a[key].get<std::string>();
}

std::vector<std::string> str_list(const json& a, const char* key) {
  if (!a.contains(key)) return {};
  if (a[key].is_string()) return {a[key].get<std::string>()};
  return a[key].get<std::vector<std::string>>();
}

}  // namespace

json McpServer::tool_list() {
  json name = prop("string", "Definition name: 'file.py:Class.method', 'module.Class.method' or a unique short name");
  json reason = prop("string", "Why: the request or design context behind this change (kept in the definition's context history)");
  return json::array({
      tool("hs_status", "Branch, uncommitted changes, locks, verification state."),
      tool("hs_find", "Search definitions by words in names, signatures and docstrings.",
           {{"query", prop("string", "Search words")}}, {"query"}),
      tool("hs_list", "List definitions, optionally under a file or name prefix.",
           {{"prefix", prop("string", "e.g. 'pkg/mod.py' or 'pkg/mod.py:Server'")}}),
      tool("hs_view", "Source code of one definition.", {{"name", name}}, {"name"}),
      tool("hs_context",
           "Everything about a definition: code, signature, comment regions, context history "
           "(original request, design notes, change reasons), linked tests and suite hash, callers, "
           "callees, commit history, lock. Start here before changing code.",
           {{"name", name}}, {"name"}),
      tool("hs_graph",
           "Query the call graph. callers/callees: direct; reach: everything it calls to depth; "
           "impact: everything that depends on it; path: call chain from name to 'to'; "
           "unused, cycles, entry: project-wide.",
           {{"query", {{"type", "string"}, {"enum", {"callers", "callees", "reach", "impact", "path", "unused", "cycles", "entry"}}}},
            {"name", name}, {"to", prop("string", "Target for 'path'")},
            {"depth", prop("integer", "Depth for reach/impact (default 3)")}},
           {"query"}),
      tool("hs_lock", "Lock definitions so no other agent can change them (expires unless renewed by activity).",
           {{"names", str_array("Definitions to lock")}}, {"names"}),
      tool("hs_unlock", "Release locks.", {{"names", str_array("Definitions to unlock")}, {"force", prop("boolean", "Break another agent's lock")}}, {"names"}),
      tool("hs_locks", "List active locks."),
      tool("hs_edit_def",
           "Replace a definition's code. The code may contain several definitions (e.g. split into "
           "helpers): the one keeping the name keeps its identity, history and tests. Locks it for "
           "you if free. Pass base (the hash from hs_view/hs_context) to detect concurrent changes.",
           {{"name", name}, {"code", prop("string", "New source code for the definition")},
            {"base", prop("string", "Hash the edit is based on (optional)")}, {"reason", reason}},
           {"name", "code", "reason"}),
      tool("hs_add_def", "Add new definitions to a file (created if missing), optionally inside a class and after a member.",
           {{"file", prop("string", "File path, e.g. 'pkg/mod.py'")}, {"container", prop("string", "Class path inside the file, e.g. 'Server'")},
            {"after", prop("string", "Member to insert after")}, {"code", prop("string", "Source code")}, {"reason", reason}},
           {"file", "code", "reason"}),
      tool("hs_delete_def", "Delete a definition.", {{"name", name}, {"reason", reason}}, {"name"}),
      tool("hs_tests", "Linked test cases of a definition, its suite hash, and suggested tests.", {{"name", name}}, {"name"}),
      tool("hs_link_tests", "Link test cases to a definition (many tests per definition).",
           {{"name", name}, {"tests", str_array("Test definitions")}}, {"name", "tests"}),
      tool("hs_unlink_tests", "Unlink test cases from a definition.", {{"name", name}, {"tests", str_array("Test definitions")}}, {"name", "tests"}),
      tool("hs_missing_tests", "Functions and methods that have no test cases."),
      tool("hs_note", "Add a comment about a line region of a definition (stored beside the code; regions may overlap).",
           {{"name", name}, {"from", prop("integer", "First line (1-based)")}, {"to", prop("integer", "Last line")}, {"text", prop("string", "The comment")}},
           {"name", "from", "to", "text"}),
      tool("hs_why", "Record context for a definition: the original request, a design snippet, or a change reason.",
           {{"name", name}, {"text", prop("string", "Context text")},
            {"kind", {{"type", "string"}, {"enum", {"origin", "design", "change", "note"}}}},
            {"source", prop("string", "Where it came from, e.g. 'docs/DESIGN.md#locks'")}},
           {"name", "text"}),
      tool("hs_diff", "Per-definition diff (line-by-line inside each). Default: last commit to working state.",
           {{"name", name}, {"from", prop("string", "work | head | head~N | @<history id> | <commit>")}, {"to", prop("string", "Same forms")}}),
      tool("hs_verify", "Check policy and run the linked tests of changed definitions. Required before push.",
           {{"all", prop("boolean", "Check every definition")}}),
      tool("hs_commit",
           "Commit the working state. The message must describe what changed and why. Returns the "
           "line-by-line diff it committed (capped; hs_show for the rest) so you can confirm it. "
           "Refused while other agents' change requests are open on definitions you changed, unless "
           "resolves lists them (make their change first) or you decline them.",
           {{"message", prop("string", "Description of the change")},
            {"resolves", {{"type", "array"}, {"items", {{"type", "integer"}}},
                          {"description", "Ids of requests this commit makes the change for"}}}},
           {"message"}),
      tool("hs_work",
           "Your work item: the feature, bug fix or chore your locks, requests and commits are for. "
           "action start (id, title for a new one, kind, priority, link), status (default: your item, "
           "locks, requests to and from you), list, done, abandon.",
           {{"action", {{"type", "string"}, {"enum", {"status", "start", "list", "done", "abandon"}}}},
            {"id", prop("string", "Work item id, e.g. FEAT-12 or BUG-7")},
            {"title", prop("string", "What it is for")},
            {"kind", {{"type", "string"}, {"enum", {"feature", "bug", "chore"}}}},
            {"priority", prop("integer", "Higher goes first in lock queues (default bug 2, feature 1, chore 0)")},
            {"link", prop("string", "Ticket URL")},
            {"all", prop("boolean", "list: include closed items")}}),
      tool("hs_request",
           "A definition you need is locked by another agent: log what you need instead of waiting. "
           "kind change (default): the holder must make your change (or decline) before committing it; "
           "give test code and its file and they become tests of the definition, so both agents' "
           "tests must pass. kind lock: queue for the lock; it is yours when released. hs_inbox "
           "tells you when it is done.",
           {{"name", name}, {"need", prop("string", "The change you need, precisely")},
            {"kind", {{"type", "string"}, {"enum", {"change", "lock"}}}},
            {"test_file", prop("string", "File for the test code, e.g. tests/test_ops.py")},
            {"test_code", prop("string", "Test functions that must pass after the change")}},
           {"name"}),
      tool("hs_requests", "Open requests to and from you (all: everyone's; closed: include finished ones).",
           {{"all", prop("boolean", "Everyone's")}, {"closed", prop("boolean", "Include finished")}}),
      tool("hs_request_respond",
           "Answer a request: accept (adds the requester's tests and links them; then make the change and "
           "hs_commit with resolves), decline (with a reason; the requester is queued for the lock), "
           "or cancel one of your own.",
           {{"id", prop("integer", "Request id")},
            {"action", {{"type", "string"}, {"enum", {"accept", "decline", "cancel"}}}},
            {"reason", prop("string", "Why (needed to decline)")}},
           {"id", "action"}),
      tool("hs_inbox",
           "Your events since you last looked: requests on what you hold, answers to yours, locks handed "
           "to you, requests done (pull to get the change). wait: seconds to wait for one.",
           {{"wait", prop("integer", "Seconds to wait if there is nothing yet (max 60)")},
            {"peek", prop("boolean", "Don't mark them as seen")}}),
      tool("hs_show",
           "Review one commit: message, author, verification, review flag (unreviewed / AI / human), "
           "line-by-line diff of every changed function and module code, per-function test summary "
           "(e.g. '18/20 passed'), and its reviews.",
           {{"commit", prop("string", "head (default), head~N, or a commit hash prefix")},
            {"max_lines", prop("integer", "Cap on diff lines returned (default 600, 0 = all)")}}),
      tool("hs_review",
           "Record your review of a commit (recorded as an AI review). Read it with hs_show first. "
           "Human approval is separate and never required; this records that an AI looked.",
           {{"commit", prop("string", "head (default), head~N, or a commit hash prefix")},
            {"verdict", {{"type", "string"}, {"enum", {"approved", "changes_requested", "comment"}}}},
            {"summary", prop("string", "What you checked and found (needed unless approving)")},
            {"comments", {{"type", "array"},
                          {"description", "Line comments: [{name, line, text}] (line within the definition)"},
                          {"items", {{"type", "object"}}}}}},
           {"verdict"}),
      tool("hs_reviews",
           "Review flags of recent commits on this branch (unreviewed, AI reviewed, human reviewed or "
           "approved), or the reviews of one commit.",
           {{"commit", prop("string", "One commit (optional)")}, {"limit", prop("integer", "Commits to list (default 20)")}}),
      tool("hs_test_summary",
           "Test results per function: '20/100 passed, 3 failed' for each, failing ones first, without "
           "listing individual tests.",
           {{"prefix", prop("string", "Only definitions under this file or name prefix")},
            {"only_problems", prop("boolean", "Only functions with failing, unrun or missing tests")}}),
      tool("hs_log", "Commit log, or the commits that changed one definition.",
           {{"name", name}, {"limit", prop("integer", "Max entries (default 20)")}}),
      tool("hs_history", "Lifecycle steps (import, update, commit, verify, rollback) with ids for rollback.",
           {{"limit", prop("integer", "Max entries (default 30)")}}),
      tool("hs_rollback", "Restore the working state of a history step. The current state stays in history.",
           {{"id", prop("integer", "History id")}}, {"id"}),
      tool("hs_render", "Generate normal source files into a folder (for building, running, or git).",
           {{"out", prop("string", "Output folder (default 'out')")}}),
      tool("hs_pull", "Fetch from the hub and fast-forward; also receives the hub's locks. Do this before starting work.",
           {{"remote", prop("string", "Remote name (default 'origin')")}}),
      tool("hs_push", "Send verified commits to the hub (run hs_verify and hs_commit first). Your locks travel with it.",
           {{"remote", prop("string", "Remote name (default 'origin')")}}),
      tool("hs_remotes", "Configured hubs (remotes)."),
      tool("hs_publish", "Render the branch into the configured git working copy, commit and push it to git.",
           {{"branch", prop("string", "Branch (default: current)")}}),
  });
}

json McpServer::call_tool(const std::string& t, const json& a) {
  Repo& r = repo_;
  auto qn = [&](const char* key = "name") { return resolve_name(r, need_str(a, key)).qname; };
  if (t == "hs_status") {
    Store& s = r.store();
    auto d = diff_namespaces(s, load_namespace(s, r.head_ns_hash()), r.work_ns());
    json locks = json::array();
    for (auto& l : list_locks(r))
      locks.push_back({{"name", l.name}, {"holder", l.holder}, {"work", l.work}, {"until", format_time(l.expires)}});
    json j = {{"branch", r.branch()}, {"agent", r.agent()}, {"changes", diff_to_json(s, d, false)},
              {"locks", locks}, {"verified", verification_status(r, r.work_ns_hash())}};
    try {
      json w = work_status(r);
      j["work"] = w["work"];
      j["requests_for_you"] = w["requests_for_you"];
      j["your_requests"] = w["your_requests"];
      j["inbox"] = w["inbox"];
    } catch (std::exception& e) {
      j["work"] = std::string("unavailable: ") + e.what();
    }
    return j;
  }
  if (t == "hs_find") return find(r, need_str(a, "query"));
  if (t == "hs_list") {
    json out = json::array();
    for (auto& row : list_defs(r, a.value("prefix", "")))
      out.push_back({{"name", row.qname}, {"kind", row.kind}, {"hash", short_hash(row.hash)}});
    return out;
  }
  if (t == "hs_view") {
    IndexRow row = resolve_name(r, need_str(a, "name"));
    return {{"name", row.qname}, {"hash", short_hash(row.hash)}, {"code", render_def(r.store(), row.hash)}};
  }
  if (t == "hs_context") return context_of(r, need_str(a, "name"));
  if (t == "hs_graph") {
    std::string q = need_str(a, "query");
    int depth = a.value("depth", 3);
    if (q == "unused") return unused(r);
    if (q == "cycles") return cycles(r);
    if (q == "entry") return entry_points(r);
    if (q == "path") return call_path(r, qn(), qn("to"));
    std::string n = qn();
    if (q == "callers") return callers(r, n);
    if (q == "callees") return callees(r, n);
    if (q == "reach") return reach(r, n, depth, false);
    if (q == "impact") return reach(r, n, depth, true);
    throw std::runtime_error("unknown graph query " + q);
  }
  if (t == "hs_lock" || t == "hs_unlock") {
    json out = json::array();
    for (auto& n : str_list(a, "names")) {
      IndexRow row = resolve_name(r, n);
      if (t == "hs_lock") {
        auto l = acquire_lock(r, row.lineage, row.qname);
        out.push_back({{"locked", row.qname}, {"until", format_time(l.expires)}});
      } else {
        out.push_back({{"unlocked", row.qname}, {"was_locked", release_lock(r, row.lineage, a.value("force", false))}});
      }
    }
    return out;
  }
  if (t == "hs_locks") {
    json out = json::array();
    for (auto& l : list_locks(r))
      out.push_back({{"name", l.name}, {"holder", l.holder}, {"work", l.work}, {"until", format_time(l.expires)}});
    return out;
  }
  if (t == "hs_work") {
    std::string action = a.value("action", "status");
    if (action == "status") return work_status(r);
    if (action == "start") {
      WorkItem w;
      w.id = need_str(a, "id");
      w.title = a.value("title", "");
      w.kind = a.value("kind", "");
      w.link = a.value("link", "");
      if (a.contains("priority")) w.priority = a["priority"].get<int>();
      return start_work(r, w).to_json();
    }
    if (action == "list") {
      json out = json::array();
      for (auto& w : coordination(r)->works(a.value("all", false))) out.push_back(w.to_json());
      return out;
    }
    if (action == "done" || action == "abandon")
      return finish_work(r, a.value("id", ""), action == "done" ? "done" : "abandoned").to_json();
    throw std::runtime_error("unknown hs_work action " + action);
  }
  if (t == "hs_request") {
    Request q = make_request(r, need_str(a, "name"), a.value("kind", "change"), a.value("need", ""),
                             a.value("test_file", ""), a.value("test_code", ""));
    json j = q.to_json();
    j["summary"] = describe_request(q);
    j["next"] = q.kind == "lock" ? "you get the lock when it is released; hs_inbox tells you"
                                 : q.holder + " is told and must deal with it before committing " + q.name +
                                       "; hs_inbox tells you when it is done";
    return j;
  }
  if (t == "hs_requests") {
    RequestFilter f;
    if (!a.value("all", false)) f.agent = r.agent();
    f.open_only = !a.value("closed", false);
    json out = json::array();
    for (auto& q : coordination(r)->requests(f)) {
      json j = q.to_json();
      j["summary"] = describe_request(q);
      out.push_back(j);
    }
    return out;
  }
  if (t == "hs_request_respond") {
    long long id = a.value("id", 0LL);
    std::string action = need_str(a, "action");
    Request q = action == "accept"    ? accept_request(r, id)
                : action == "decline" ? decline_request(r, id, a.value("reason", ""))
                : action == "cancel"  ? cancel_request(r, id)
                                      : throw std::runtime_error("action must be accept, decline or cancel");
    json j = q.to_json();
    j["summary"] = describe_request(q);
    if (action == "accept") j["next"] = "make the change, hs_verify, then hs_commit with resolves [" + std::to_string(id) + "]";
    return j;
  }
  if (t == "hs_inbox") {
    json out = json::array();
    for (auto& e : inbox(r, !a.value("peek", false), std::min(a.value("wait", 0), 60))) {
      json j = e.to_json();
      j["text"] = describe_event(e);
      out.push_back(j);
    }
    return out;
  }
  if (t == "hs_edit_def") {
    IndexRow row = resolve_name(r, need_str(a, "name"));
    if (!lock_on(r, row.lineage)) acquire_lock(r, row.lineage, row.qname);
    Namespace ns = r.work_ns();
    EditOutcome o = replace_def(r, ns, row.qname, need_str(a, "code"), a.value("base", ""), need_str(a, "reason"));
    if (o.ok) commit_edits(r, ns, {&o}, need_str(a, "reason"));
    return outcome_json(o);
  }
  if (t == "hs_add_def") {
    Namespace ns = r.work_ns();
    EditOutcome o = add_def(r, ns, need_str(a, "file"), a.value("container", ""), a.value("after", ""),
                            need_str(a, "code"), need_str(a, "reason"));
    if (o.ok) commit_edits(r, ns, {&o}, need_str(a, "reason"));
    return outcome_json(o);
  }
  if (t == "hs_delete_def") {
    Namespace ns = r.work_ns();
    EditOutcome o = delete_def(r, ns, qn());
    if (o.ok) commit_edits(r, ns, {&o}, a.value("reason", ""));
    return outcome_json(o);
  }
  if (t == "hs_tests") return tests_info(r, need_str(a, "name"));
  if (t == "hs_link_tests" || t == "hs_unlink_tests") {
    std::string n = qn();
    if (t == "hs_link_tests") link_tests(r, n, str_list(a, "tests"));
    else unlink_tests(r, n, str_list(a, "tests"));
    return tests_info(r, n);
  }
  if (t == "hs_missing_tests") {
    json out = json::array();
    for (auto& w : missing_tests(r, r.work_ns())) out.push_back(w.qname);
    return out;
  }
  if (t == "hs_note") {
    Namespace ns = r.work_ns();
    std::string n = qn();
    std::string id = add_note(r, ns, n, a.value("from", 0), a.value("to", 0), need_str(a, "text"));
    r.set_work_ns(ns, "note", "note on " + n);
    return {{"note", id}, {"name", n}};
  }
  if (t == "hs_why") {
    Namespace ns = r.work_ns();
    std::string n = qn();
    std::string kind = a.value("kind", "note");
    add_context(r, ns, n, kind, need_str(a, "text"), a.value("source", ""));
    r.set_work_ns(ns, "context", kind + " context for " + n);
    return {{"recorded", kind}, {"name", n}};
  }
  if (t == "hs_diff") {
    Store& s = r.store();
    auto d = diff_namespaces(s, load_namespace(s, resolve_ns(r, a.value("from", "head"))),
                             load_namespace(s, resolve_ns(r, a.value("to", "work"))));
    if (a.contains("name")) {
      IndexRow row = resolve_name(r, need_str(a, "name"));
      std::erase_if(d.defs, [&](const DefChange& c) { return c.lineage != row.lineage; });
      d.files_added.clear();
      d.files_removed.clear();
      d.files_glue_changed.clear();
      d.module_changes.clear();
      d.tests_changed = false;
    }
    return diff_to_json(s, d, true);
  }
  if (t == "hs_verify") return verify(r, a.value("all", false));
  if (t == "hs_commit") {
    std::vector<long long> resolves;
    for (auto& id : a.value("resolves", json::array())) resolves.push_back(id.get<long long>());
    auto wc = commit_work(r, need_str(a, "message"), resolves);
    auto& c = wc.result;
    json j = {{"commit", short_hash(c.commit)}, {"changes", diff_to_json(r.store(), c.diff, true, 300)}};
    if (!wc.resolved.empty()) {
      json res = json::array();
      for (auto& q : wc.resolved) res.push_back(describe_request(q));
      j["resolved"] = res;
      j["requests"] = wc.landed ? "done: the requesters are told" : "done when you push: the requesters are told then";
    }
    if (!wc.warning.empty()) j["warning"] = wc.warning;
    if (!wc.left_open.empty()) {
      json open = json::array();
      for (auto& q : wc.left_open) open.push_back(describe_request(q));
      j["still_open"] = open;
    }
    if (verification_status(r, r.work_ns_hash()) != "passed") j["note"] = "not verified: run hs_verify before pushing";
    j["review"] = "check the diff above is what you intended; hs_review records your review";
    return j;
  }
  if (t == "hs_show") return show_commit(r, resolve_commit(r, a.value("commit", "head")), a.value("max_lines", 600));
  if (t == "hs_review") {
    Review rv = add_review(r, a.value("commit", "head"), need_str(a, "verdict"), a.value("summary", ""), "ai",
                           a.value("comments", json::array()));
    json j = {{"recorded", review_to_json(rv)}, {"flag", review_status(r.store(), rv.commit).to_json()}};
    std::string note = share_reviews(r);
    if (!note.empty()) j["note"] = note;
    return j;
  }
  if (t == "hs_reviews") {
    if (a.contains("commit")) {
      Hash h = resolve_commit(r, a["commit"]);
      json rs = json::array();
      for (auto& rv : reviews_of(r.store(), h)) rs.push_back(review_to_json(rv));
      return {{"commit", h}, {"flag", review_status(r.store(), h).to_json()}, {"reviews", rs}};
    }
    json out = json::array();
    for (auto& h : branch_commits(r, r.branch(), a.value("limit", 20))) {
      json row = commit_row(r, h);
      out.push_back({{"commit", row["short"]}, {"message", row["message"]}, {"author", row["author"]},
                     {"when", row["when"]}, {"verification", row["verification"]}, {"review", row["review"]["label"]}});
    }
    return out;
  }
  if (t == "hs_test_summary") {
    std::string prefix = a.value("prefix", "");
    bool problems = a.value("only_problems", false);
    json out = json::array();
    int funcs = 0, total = 0, passed = 0, failed = 0;
    std::vector<json> rows;
    for (auto& [q, ts] : test_summaries(r, r.work_ns())) {
      if (!prefix.empty() && !starts_with(q, prefix)) continue;
      if (ts.total == 0 && !ts.needs_tests) continue;
      ++funcs, total += ts.total, passed += ts.passed, failed += ts.failed;
      bool ok = ts.total > 0 && ts.passed == ts.total;
      if (problems && ok) continue;
      json j = ts.to_json();
      j["name"] = q;
      rows.push_back(j);
    }
    // Failing first, then not run / missing, then passing.
    auto rank = [](const json& j) { return j["failed"] > 0 ? 0 : j["total"] == 0 ? 1 : j["not_run"] > 0 ? 2 : 3; };
    std::stable_sort(rows.begin(), rows.end(), [&](auto& x, auto& y) { return rank(x) < rank(y); });
    for (auto& j : rows) out.push_back(j);
    return {{"functions", funcs}, {"tests", total}, {"passed", passed}, {"failed", failed}, {"rows", out}};
  }
  if (t == "hs_log") return log(r, a.value("limit", 20), a.contains("name") ? qn() : "");
  if (t == "hs_history") {
    json out = json::array();
    for (auto& e : history(r, a.value("limit", 30)))
      out.push_back({{"id", e.id}, {"time", format_time(e.time)}, {"step", e.step}, {"agent", e.agent}, {"message", e.message}});
    return out;
  }
  if (t == "hs_rollback") {
    auto e = rollback(r, a.value("id", 0LL));
    return {{"restored", e.id}, {"step", e.step}, {"note", "the previous state is still in hs_history"}};
  }
  if (t == "hs_render") {
    auto res = render_to(r, r.root() / from_utf8(a.value("out", "out")));
    return {{"written", res.written}, {"unchanged", res.unchanged}, {"deleted", res.deleted}};
  }
  if (t == "hs_pull" || t == "hs_push") {
    std::string remote = a.value("remote", "origin");
    auto hub = open_remote(r, remote);
    return t == "hs_pull" ? pull(r, *hub, remote) : push(r, *hub, remote);
  }
  if (t == "hs_remotes") {
    json out = json::object();
    for (auto& [k, v] : r.store().refs_with_prefix("remote/"))
      if (std::count(k.begin(), k.end(), '/') == 1) out[k.substr(7)] = v;
    return out;
  }
  if (t == "hs_publish") return publish(r, a.value("branch", ""));
  throw std::runtime_error("unknown tool " + t);
}

json McpServer::handle(const json& msg) {
  json id = msg.contains("id") ? msg["id"] : json();
  std::string method = msg.value("method", "");
  auto reply = [&](json result) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}; };
  auto fail = [&](int code, const std::string& m) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", m}}}};
  };
  if (!msg.contains("id")) return nullptr;  // notification
  if (method == "initialize") {
    // Each MCP client session acts as its own agent unless HS_AGENT is set.
    const char* env = std::getenv("HS_AGENT");
    if (!(env && *env)) {
      std::string client = msg.value("/params/clientInfo/name"_json_pointer, std::string("agent"));
      repo_.set_agent(client + "-" + new_uuid().substr(0, 6));
    }
    std::string ver = msg.value("/params/protocolVersion"_json_pointer, std::string("2025-06-18"));
    json caps = {{"tools", json::object()}};
    if (channel_) caps["experimental"] = {{"claude/channel", json::object()}};
    return reply({{"protocolVersion", ver},
                  {"capabilities", caps},
                  {"serverInfo", {{"name", "hashstash"}, {"version", HS_VERSION}}},
                  {"instructions", kInstructions}});
  }
  if (method == "ping") return reply(json::object());
  if (method == "tools/list") return reply({{"tools", tool_list()}});
  if (method == "tools/call") {
    std::string name = msg.value("/params/name"_json_pointer, std::string());
    json args = msg.contains("params") && msg["params"].contains("arguments") ? msg["params"]["arguments"] : json::object();
    try {
      json result = call_tool(name, args);
      renew_locks(repo_);
      return reply({{"content", json::array({{{"type", "text"}, {"text", result.dump(2)}}})}, {"isError", false}});
    } catch (std::exception& e) {
      return reply({{"content", json::array({{{"type", "text"}, {"text", e.what()}}})}, {"isError", true}});
    }
  }
  return fail(-32601, "method not found: " + method);
}

void McpServer::serve(std::istream& in, std::ostream& out) {
  std::mutex out_mu;
  auto write = [&](const json& j) {
    std::lock_guard<std::mutex> g(out_mu);
    out << j.dump() << "\n";
    out.flush();
  };
  // Channel: a watcher with its own connection (the hub, or its own handle on
  // this store) waits for the agent's events and pushes each one.
  auto stop = std::make_shared<std::atomic<bool>>(false);
  bool watching = false;
  auto start_watch = [&] {
    if (!channel_ || watching) return;
    watching = true;
    std::string agent = repo_.agent();
    std::shared_ptr<Coordination> c;
    std::shared_ptr<Store> own_store;
    if (coordinated_by_hub(repo_)) {
      c = coordination(repo_);
    } else {
      own_store = std::make_shared<Store>(to_generic(repo_.hs_dir() / "store.db"));
      c = sql_coordination(*own_store);
    }
    long long after = c->last_event();
    std::thread([c, own_store, agent, after, stop, write]() mutable {
      while (!*stop) {
        std::vector<Event> events;
        try {
          events = c->events_wait(agent, after, 50, 25);
        } catch (...) {
          std::this_thread::sleep_for(std::chrono::seconds(5));
          continue;
        }
        for (auto& e : events) {
          if (*stop) return;
          after = e.seq;
          Request q = Request::from_json(e.data.value("request", json::object()));
          write({{"jsonrpc", "2.0"},
                 {"method", "notifications/claude/channel"},
                 {"params", {{"content", "hashstash: " + describe_event(e)},
                             {"meta", {{"kind", e.kind}, {"request", std::to_string(q.id)}, {"seq", std::to_string(e.seq)}}}}}});
        }
      }
    }).detach();
  };
  std::string line;
  while (std::getline(in, line)) {
    if (trim(line).empty()) continue;
    json resp;
    bool init = false;
    try {
      json msg = json::parse(line);
      init = msg.value("method", "") == "initialize";
      resp = handle(msg);
    } catch (std::exception& e) {
      resp = {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", e.what()}}}};
    }
    if (!resp.is_null()) write(resp);
    if (init) start_watch();  // the agent's name is known from here on
  }
  *stop = true;
}

}  // namespace hs
