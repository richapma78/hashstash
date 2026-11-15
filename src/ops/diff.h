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

// A file whose module-level code (imports, constants, code between
// definitions) was added, removed or changed. Entries are empty when absent.
struct FileChange {
  std::string path;
  std::string kind;  // added | removed | changed
  FileEntry old_entry, new_entry;
};

struct NamespaceDiff {
  std::vector<DefChange> defs;
  std::vector<std::string> files_added, files_removed, files_glue_changed;
  std::vector<FileChange> module_changes;  // with text, for line diffs
  bool tests_changed = false;
  bool empty() const {
    return defs.empty() && files_added.empty() && files_removed.empty() &&
           files_glue_changed.empty() && !tests_changed;
  }
};

NamespaceDiff diff_namespaces(const Store& s, const Namespace& a, const Namespace& b);
// with_text adds line diffs (and the full text of added/removed definitions).
// max_lines > 0 caps the diff lines returned; the rest is listed by name only
// and "truncated" says how much was left out.
nlohmann::json diff_to_json(const Store& s, const NamespaceDiff& d, bool with_text, int max_lines = 0);
std::string diff_to_text(const Store& s, const NamespaceDiff& d, bool with_text);

// A definition's own text, with child definitions shown as one placeholder line.
std::string own_text(const Store& s, const Hash& def);
// A file's module-level code, with each definition shown as one placeholder line.
std::string module_text(const Store& s, const FileEntry& e);
// Text of a definition change's old and new versions ("" when absent).
std::pair<std::string, std::string> change_texts(const Store& s, const DefChange& c);
std::pair<std::string, std::string> change_texts(const Store& s, const FileChange& c);

}  // namespace hs
