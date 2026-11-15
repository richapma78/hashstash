#include "lang/lang.h"

#include <algorithm>

namespace hs {

std::unique_ptr<Language> make_python();

const std::vector<std::unique_ptr<Language>>& languages() {
  static std::vector<std::unique_ptr<Language>> all = [] {
    std::vector<std::unique_ptr<Language>> v;
    v.push_back(make_python());
    return v;
  }();
  return all;
}

const Language* language_for_path(const std::string& path) {
  for (auto& l : languages())
    if (l->handles(path)) return l.get();
  return nullptr;
}

const Language* language_by_name(const std::string& name) {
  for (auto& l : languages())
    if (l->name() == name) return l.get();
  return nullptr;
}

std::vector<size_t> line_starts(std::string_view src) {
  std::vector<size_t> v{0};
  for (size_t i = 0; i < src.size(); ++i)
    if (src[i] == '\n') v.push_back(i + 1);
  return v;
}

// Removes the longest whitespace prefix shared by all non-blank lines.
std::string dedent(std::string_view text, std::string* removed_prefix) {
  std::string prefix;
  bool first = true;
  size_t i = 0;
  while (i < text.size()) {
    size_t eol = text.find('\n', i);
    if (eol == std::string_view::npos) eol = text.size();
    std::string_view line = text.substr(i, eol - i);
    size_t ws = line.find_first_not_of(" \t");
    bool blank = ws == std::string_view::npos || line[ws] == '\r';
    if (!blank) {
      std::string_view lead = line.substr(0, ws);
      if (first) {
        prefix = std::string(lead);
        first = false;
      } else {
        size_t k = 0;
        while (k < prefix.size() && k < lead.size() && prefix[k] == lead[k]) ++k;
        prefix.resize(k);
      }
    }
    i = eol + 1;
  }
  if (removed_prefix) *removed_prefix = prefix;
  if (prefix.empty()) return std::string(text);
  std::string out;
  out.reserve(text.size());
  i = 0;
  while (i < text.size()) {
    size_t eol = text.find('\n', i);
    size_t end = eol == std::string_view::npos ? text.size() : eol + 1;
    std::string_view line = text.substr(i, end - i);
    if (line.substr(0, prefix.size()) == prefix) line.remove_prefix(prefix.size());
    else {  // blank line shorter than the prefix
      size_t ws = line.find_first_not_of(" \t");
      if (ws != std::string_view::npos) line.remove_prefix(ws);
    }
    out += line;
    i = end;
  }
  return out;
}

// Adds prefix to every non-blank line.
std::string indent(std::string_view text, const std::string& prefix) {
  if (prefix.empty()) return std::string(text);
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    size_t eol = text.find('\n', i);
    size_t end = eol == std::string_view::npos ? text.size() : eol + 1;
    std::string_view line = text.substr(i, end - i);
    bool blank = line.find_first_not_of(" \t\r\n") == std::string_view::npos;
    if (!blank) out += prefix;
    out += line;
    i = end;
  }
  return out;
}

std::string collapse_ws(std::string_view s) {
  std::string out;
  bool space = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      space = !out.empty();
    } else {
      if (space) out.push_back(' ');
      out.push_back(c);
      space = false;
    }
  }
  return out;
}

}  // namespace hs
