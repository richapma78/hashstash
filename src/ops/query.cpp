#include "ops/query.h"

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "ops/tests.h"

namespace hs {

static const char* kRowCols = "qname, short, file, hash, lineage, kind, lang, parent, seq";

static IndexRow read_row(Stmt& st) {
  return IndexRow{st.text(0), st.text(1), st.text(2), st.text(3), st.text(4),
                  st.text(5), st.text(6), st.text(7), (int)st.integer(8)};
}

static std::vector<IndexRow> query_rows(Repo& repo, const std::string& where,
                                        const std::vector<std::string>& args) {
  std::string sql = std::string("SELECT ") + kRowCols + " FROM name_index WHERE " + where;
  Stmt st(repo.store().db(), sql.c_str());
  for (size_t i = 0; i < args.size(); ++i) st.bind((int)i + 1, args[i]);
  std::vector<IndexRow> out;
  while (st.step()) out.push_back(read_row(st));
  return out;
}

std::optional<IndexRow> row_by_qname(Repo& repo, const std::string& qname) {
  auto rows = query_rows(repo, "qname = ?", {qname});
  if (rows.empty()) return std::nullopt;
  return rows[0];
}

std::vector<IndexRow> rows_by_lineage(Repo& repo, const std::string& lineage) {
  return query_rows(repo, "lineage = ?", {lineage});
}

// "pkg/mod.py" + "Server.start" -> "pkg.mod.Server.start"
static std::string dotted(const IndexRow& r) {
  std::string m = r.file;
  size_t dot = m.rfind('.');
  if (dot != std::string::npos && m.find('/', dot) == std::string::npos) m.resize(dot);
  for (auto& c : m)
    if (c == '/') c = '.';
  return m + "." + r.qname.substr(r.qname.find(':') + 1);
}

std::vector<IndexRow> lookup(Repo& repo, const std::string& q) {
  if (q.empty()) return {};
  if (q[0] == '#') return query_rows(repo, "hash >= ? AND hash < ? AND kind != 'module'", {q.substr(1), q.substr(1) + "\x7f"});
  if (q.find(':') != std::string::npos) {
    auto rows = query_rows(repo, "qname = ?", {q});
    if (!rows.empty()) return rows;
  }
  std::string last = q.substr(q.rfind('.') + 1);
  std::string base = last.substr(0, last.find('~'));
  auto cands = query_rows(repo, "short = ? AND kind != 'module'", {base});
  std::vector<IndexRow> exact_path, suffix;
  for (auto& r : cands) {
    std::string path = r.qname.substr(r.qname.find(':') + 1);
    std::string d = dotted(r);
    if (path == q) exact_path.push_back(r);
    else if (d == q || ends_with(d, "." + q)) suffix.push_back(r);
  }
  return exact_path.empty() ? suffix : exact_path;
}

IndexRow resolve_name(Repo& repo, const std::string& q) {
  auto rows = lookup(repo, q);
  if (rows.size() == 1) return rows[0];
  if (rows.empty()) throw std::runtime_error("no definition named '" + q + "'");
  std::string msg = "'" + q + "' is ambiguous; use one of:";
  for (size_t i = 0; i < rows.size() && i < 12; ++i) msg += "\n  " + rows[i].qname;
  throw std::runtime_error(msg);
}

std::vector<IndexRow> list_defs(Repo& repo, const std::string& prefix) {
  if (prefix.empty()) return query_rows(repo, "kind != 'module' ORDER BY file, seq", {});
  return query_rows(repo, "kind != 'module' AND qname >= ? AND qname < ? ORDER BY file, seq",
                    {prefix, prefix + "\x7f"});
}

json def_summary(Repo& repo, const IndexRow& r) {
  json j = {{"name", r.qname}, {"kind", r.kind}, {"file", r.file}, {"hash", short_hash(r.hash)}};
  Stmt st(repo.store().db(), "SELECT signature, doc, lines FROM def_info WHERE hash = ?");
  st.bind(1, r.hash);
  if (st.step()) {
    j["signature"] = st.text(0);
    if (!st.text(1).empty()) j["doc"] = st.text(1);
    j["lines"] = st.integer(2);
  }
  return j;
}

json find(Repo& repo, const std::string& text, int limit) {
  // Turn free text into an FTS5 prefix query: each word must match.
  std::string fq;
  std::string word;
  auto flush = [&] {
    if (word.empty()) return;
    if (!fq.empty()) fq += " ";
    fq += "\"" + word + "\"*";
    word.clear();
  };
  for (char c : text) {
    if (std::isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80) word += c;
    else flush();
  }
  flush();
  json out = json::array();
  if (fq.empty()) return out;
  Stmt st(repo.store().db(),
          "SELECT qname FROM name_fts WHERE name_fts MATCH ? ORDER BY rank LIMIT ?");
  st.bind(1, fq).bind(2, (long long)limit);
  while (st.step())
    if (auto r = row_by_qname(repo, st.text(0))) out.push_back(def_summary(repo, *r));
  return out;
}

// ------------------------------------------------------------------ graph

json callers(Repo& repo, const std::string& qname) {
  Stmt st(repo.store().db(),
          "SELECT caller, name, line, confidence FROM edge WHERE callee = ? ORDER BY caller, line");
  st.bind(1, qname);
  json out = json::array();
  while (st.step())
    out.push_back({{"caller", st.text(0)}, {"as", st.text(1)}, {"line", st.integer(2)},
                   {"confidence", st.text(3)}});
  return out;
}

json callees(Repo& repo, const std::string& qname) {
  Stmt st(repo.store().db(),
          "SELECT callee, name, line, confidence FROM edge WHERE caller = ? ORDER BY line");
  st.bind(1, qname);
  json out = json::array();
  while (st.step()) {
    json e = {{"call", st.text(1)}, {"line", st.integer(2)}, {"confidence", st.text(3)}};
    if (!st.text(0).empty()) e["callee"] = st.text(0);
    out.push_back(e);
  }
  return out;
}

using Adj = std::unordered_map<std::string, std::vector<std::string>>;

// Edges good enough to follow: exact or likely.
static Adj adjacency(Repo& repo, bool reverse) {
  Adj adj;
  Stmt st(repo.store().db(),
          "SELECT DISTINCT caller, callee FROM edge WHERE callee != '' AND confidence IN ('exact','likely')");
  while (st.step()) {
    if (reverse) adj[st.text(1)].push_back(st.text(0));
    else adj[st.text(0)].push_back(st.text(1));
  }
  return adj;
}

json reach(Repo& repo, const std::string& qname, int depth, bool reverse) {
  Adj adj = adjacency(repo, reverse);
  std::map<std::string, int> seen{{qname, 0}};
  std::deque<std::string> q{qname};
  json levels = json::array();
  while (!q.empty()) {
    std::string n = q.front();
    q.pop_front();
    int d = seen[n];
    if (d >= depth) continue;
    for (auto& m : adj[n]) {
      if (seen.count(m)) continue;
      seen[m] = d + 1;
      q.push_back(m);
      while ((int)levels.size() < d + 1) levels.push_back(json::array());
      levels[d].push_back({{"name", m}, {"from", n}});
    }
  }
  return {{"root", qname}, {"direction", reverse ? "callers" : "callees"}, {"levels", levels},
          {"count", seen.size() - 1}};
}

json call_path(Repo& repo, const std::string& from, const std::string& to, int max_depth) {
  Adj adj = adjacency(repo, false);
  std::unordered_map<std::string, std::string> prev;
  std::unordered_map<std::string, int> dist{{from, 0}};
  std::deque<std::string> q{from};
  while (!q.empty()) {
    std::string n = q.front();
    q.pop_front();
    if (n == to) break;
    if (dist[n] >= max_depth) continue;
    for (auto& m : adj[n]) {
      if (dist.count(m)) continue;
      dist[m] = dist[n] + 1;
      prev[m] = n;
      q.push_back(m);
    }
  }
  if (!dist.count(to)) return {{"from", from}, {"to", to}, {"path", nullptr}};
  std::vector<std::string> path{to};
  while (path.back() != from) path.push_back(prev[path.back()]);
  return {{"from", from}, {"to", to}, {"path", json(std::vector<std::string>(path.rbegin(), path.rend()))}};
}

static bool is_dunder(const std::string& s) {
  return s.size() > 4 && starts_with(s, "__") && ends_with(s, "__");
}

json unused(Repo& repo) {
  std::set<std::string> called;
  {
    Stmt st(repo.store().db(), "SELECT DISTINCT callee FROM edge WHERE callee != '' AND callee != caller");
    while (st.step()) called.insert(st.text(0));
  }
  json out = json::array();
  for (auto& r : list_defs(repo, "")) {
    if (r.kind == "class" || called.count(r.qname)) continue;
    if (is_dunder(r.short_name) || r.short_name == "main" || is_test_def(r.file, r.qname)) continue;
    out.push_back({{"name", r.qname}, {"kind", r.kind}});
  }
  return out;
}

json cycles(Repo& repo) {
  Adj adj = adjacency(repo, false);
  // Tarjan's strongly connected components.
  std::unordered_map<std::string, int> index, low;
  std::unordered_map<std::string, bool> on;
  std::vector<std::string> stack;
  int counter = 0;
  json out = json::array();
  std::function<void(const std::string&)> strong = [&](const std::string& v) {
    index[v] = low[v] = counter++;
    stack.push_back(v);
    on[v] = true;
    for (auto& w : adj[v]) {
      if (!index.count(w)) {
        strong(w);
        low[v] = std::min(low[v], low[w]);
      } else if (on[w]) {
        low[v] = std::min(low[v], index[w]);
      }
    }
    if (low[v] == index[v]) {
      std::vector<std::string> comp;
      std::string w;
      do {
        w = stack.back();
        stack.pop_back();
        on[w] = false;
        comp.push_back(w);
      } while (w != v);
      bool self = comp.size() == 1 &&
                  std::find(adj[v].begin(), adj[v].end(), v) != adj[v].end();
      if (comp.size() > 1 || self) out.push_back(comp);
    }
  };
  std::vector<std::string> nodes;
  for (auto& [k, v] : adj) nodes.push_back(k);
  for (auto& n : nodes)
    if (!index.count(n)) strong(n);
  return out;
}

json entry_points(Repo& repo) {
  json out = json::array();
  Stmt st(repo.store().db(),
          "SELECT DISTINCT n.qname, n.kind, n.short FROM name_index n "
          "WHERE (n.kind = 'module' AND EXISTS (SELECT 1 FROM edge e WHERE e.caller = n.qname)) "
          "   OR n.short = 'main' ORDER BY n.qname");
  while (st.step()) out.push_back({{"name", st.text(0)}, {"kind", st.text(1)}});
  return out;
}

}  // namespace hs
