#include "mcp/server.h"

#include <cstdlib>
#include <string>

#include "ops/context.h"
#include "ops/diff.h"
#include "ops/edit.h"
#include "ops/history.h"
#include "ops/import_render.h"
#include "ops/locks.h"
#include "ops/query.h"
#include "ops/tests.h"
#include "ops/verify.h"

namespace hs {

using json = nlohmann::json;

static const char* kInstructions =
    "hashstash is a version control system for AI/SI agentic coding. Code is stored per "
    "definition (function, method, class), not per file, and normal source files are generated "
    "with hs_render. Workflow: hs_find / hs_context to understand a definition (code, notes, "
    "context history, tests, callers, callees, change log); hs_lock it; hs_edit_def with the new "
    "code and a reason; add or link test cases (every function and method needs its own tests); "
    "hs_verify; hs_commit with a description of what changed. Keep definitions small: responses "
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
      tool("hs_commit", "Commit the working state. The message must describe what changed and why.",
           {{"message", prop("string", "Description of the change")}}, {"message"}),
      tool("hs_log", "Commit log, or the commits that changed one definition.",
           {{"name", name}, {"limit", prop("integer", "Max entries (default 20)")}}),
      tool("hs_history", "Lifecycle steps (import, update, commit, verify, rollback) with ids for rollback.",
           {{"limit", prop("integer", "Max entries (default 30)")}}),
      tool("hs_rollback", "Restore the working state of a history step. The current state stays in history.",
           {{"id", prop("integer", "History id")}}, {"id"}),
      tool("hs_render", "Generate normal source files into a folder (for building, running, or git).",
           {{"out", prop("string", "Output folder (default 'out')")}}),
  });
}

json McpServer::call_tool(const std::string& t, const json& a) {
  Repo& r = repo_;
  auto qn = [&](const char* key = "name") { return resolve_name(r, need_str(a, key)).qname; };
  if (t == "hs_status") {
    Store& s = r.store();
    auto d = diff_namespaces(s, load_namespace(s, r.head_ns_hash()), r.work_ns());
    json locks = json::array();
    for (auto& l : list_locks(r)) locks.push_back({{"name", l.name}, {"holder", l.holder}, {"until", format_time(l.expires)}});
    return {{"branch", r.branch()}, {"agent", r.agent()}, {"changes", diff_to_json(s, d, false)},
            {"locks", locks}, {"verified", verification_status(r, r.work_ns_hash())}};
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
    for (auto& l : list_locks(r)) out.push_back({{"name", l.name}, {"holder", l.holder}, {"until", format_time(l.expires)}});
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
    }
    return diff_to_json(s, d, true);
  }
  if (t == "hs_verify") return verify(r, a.value("all", false));
  if (t == "hs_commit") {
    auto c = commit(r, need_str(a, "message"));
    json j = {{"commit", short_hash(c.commit)}, {"changes", diff_to_json(r.store(), c.diff, false)}};
    if (verification_status(r, r.work_ns_hash()) != "passed") j["note"] = "not verified: run hs_verify before pushing";
    return j;
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
    return reply({{"protocolVersion", ver},
                  {"capabilities", {{"tools", json::object()}}},
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
  std::string line;
  while (std::getline(in, line)) {
    if (trim(line).empty()) continue;
    json resp;
    try {
      resp = handle(json::parse(line));
    } catch (std::exception& e) {
      resp = {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", e.what()}}}};
    }
    if (!resp.is_null()) {
      out << resp.dump() << "\n";
      out.flush();
    }
  }
}

}  // namespace hs
