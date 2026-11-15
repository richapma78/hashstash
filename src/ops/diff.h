#pragma once
// Diffs: line-level (Myers) and per-definition between two namespaces.
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "core/model.h"

namespace hs {

struct LineOp {
  char op;  // ' ' same, '-' removed, '+' added
  std::string text;
  int a = 0, b = 0;  // 0-based line numbers in old/new (-1 if absent)
};

std::vector<LineOp> diff_lines(const std::vector<std::string>& a, const std::vector<std::string>& b);
// Unified diff text with context lines; "" when equal.
std::string unified_diff(const std::string& a, const std::string& b, int context = 3);
// For each old line, its new line number, or -1 if removed.
std::vector<int> line_map(const std::string& a, const std::string& b);

struct DefChange {
  std::string kind;  // added | removed | changed | renamed | formatting | moved-file
  std::string old_name, new_name;
  Hash old_hash, new_hash;
  std::string lineage;
};

struct NamespaceDiff {
  std::vector<DefChange> defs;
  std::vector<std::string> files_added, files_removed, files_glue_changed;
  bool tests_changed = false;
  bool empty() const {
    return defs.empty() && files_added.empty() && files_removed.empty() &&
           files_glue_changed.empty() && !tests_changed;
  }
};

NamespaceDiff diff_namespaces(const Store& s, const Namespace& a, const Namespace& b);
nlohmann::json diff_to_json(const Store& s, const NamespaceDiff& d, bool with_text);
std::string diff_to_text(const Store& s, const NamespaceDiff& d, bool with_text);

}  // namespace hs
