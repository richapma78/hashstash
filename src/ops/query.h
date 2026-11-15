#pragma once
// Read-side queries over the index: name lookup, search, call graph.
// Every query returns JSON so the CLI and MCP share one shape.
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "ops/repo.h"

namespace hs {

using json = nlohmann::json;

struct IndexRow {
  std::string qname, short_name, file, hash, lineage, kind, lang, parent;
  int seq = 0;
};

// Accepts "pkg/mod.py:Server.start", "pkg.mod.Server.start", "Server.start",
// "start" (if unique) or "#hashprefix". Throws with candidates if ambiguous.
IndexRow resolve_name(Repo& repo, const std::string& query);
std::vector<IndexRow> lookup(Repo& repo, const std::string& query);  // all candidates
std::optional<IndexRow> row_by_qname(Repo& repo, const std::string& qname);
std::vector<IndexRow> list_defs(Repo& repo, const std::string& prefix);
std::vector<IndexRow> rows_by_lineage(Repo& repo, const std::string& lineage);

json def_summary(Repo& repo, const IndexRow& r);  // name, kind, file, signature, doc, lines...
json find(Repo& repo, const std::string& text, int limit = 30);

// Call graph. Confidence: exact | likely | ambiguous | external.
json callers(Repo& repo, const std::string& qname);
json callees(Repo& repo, const std::string& qname);
// Breadth-first from qname following callees (or callers when reverse) to depth.
json reach(Repo& repo, const std::string& qname, int depth, bool reverse);
json call_path(Repo& repo, const std::string& from, const std::string& to, int max_depth = 12);
json unused(Repo& repo);
json cycles(Repo& repo);
json entry_points(Repo& repo);

}  // namespace hs
