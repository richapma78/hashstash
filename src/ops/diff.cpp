#include "ops/diff.h"

#include <map>
#include <set>

#include "core/util.h"
#include "ops/build.h"

namespace hs {

// Myers O(ND) diff with a full trace for backtracking.
std::vector<LineOp> diff_lines(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  int n = (int)a.size(), m = (int)b.size(), max = n + m;
  std::vector<LineOp> ops;
  if (max == 0) return ops;
  std::vector<int> v(2 * max + 2, 0);
  std::vector<std::vector<int>> trace;
  int off = max + 1;
  bool done = false;
  for (int d = 0; d <= max && !done; ++d) {
    trace.push_back(v);
    for (int k = -d; k <= d; k += 2) {
      int x = (k == -d || (k != d && v[off + k - 1] < v[off + k + 1])) ? v[off + k + 1] : v[off + k - 1] + 1;
      int y = x - k;
      while (x < n && y < m && a[x] == b[y]) ++x, ++y;
      v[off + k] = x;
      if (x >= n && y >= m) {
        done = true;
        break;
      }
    }
  }
  int x = n, y = m;
  for (int d = (int)trace.size() - 1; d >= 0; --d) {
    auto& vv = trace[d];
    int k = x - y;
    int pk = (k == -d || (k != d && vv[off + k - 1] < vv[off + k + 1])) ? k + 1 : k - 1;
    int px = vv[off + pk], py = px - pk;
    while (x > px && y > py) {
      --x, --y;
      ops.push_back({' ', a[x], x, y});
    }
    if (d > 0) {
      if (x == px) ops.push_back({'+', b[--y], -1, y});
      else ops.push_back({'-', a[--x], x, -1});
    }
    x = px, y = py;
  }
  return {ops.rbegin(), ops.rend()};
}

static std::vector<std::string> lines_lf(const std::string& s) {
  std::vector<std::string> out;
  for (auto& l : split_lines(s)) {
    std::string t = l;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    out.push_back(t);
  }
  return out;
}

std::vector<int> line_map(const std::string& a, const std::string& b) {
  auto ops = diff_lines(lines_lf(a), lines_lf(b));
  std::vector<int> map(lines_lf(a).size(), -1);
  for (auto& o : ops)
    if (o.op == ' ') map[o.a] = o.b;
  return map;
}

std::string unified_diff(const std::string& a, const std::string& b, int context) {
  auto ops = diff_lines(lines_lf(a), lines_lf(b));
  std::string out;
  size_t i = 0;
  while (i < ops.size()) {
    if (ops[i].op == ' ') {
      ++i;
      continue;
    }
    // Hunk: extend while changes are within 2*context of each other.
    size_t start = i >= (size_t)context ? i - context : 0;
    size_t end = i;
    size_t last_change = i;
    while (end < ops.size()) {
      if (ops[end].op != ' ') last_change = end;
      else if (end - last_change > (size_t)(2 * context)) break;
      ++end;
    }
    end = std::min(ops.size(), last_change + context + 1);
    int a0 = -1, b0 = -1, ac = 0, bc = 0;
    for (size_t j = start; j < end; ++j) {
      if (ops[j].op != '+') {
        if (a0 < 0) a0 = ops[j].a;
        ++ac;
      }
      if (ops[j].op != '-') {
        if (b0 < 0) b0 = ops[j].b;
        ++bc;
      }
    }
    out += "@@ -" + std::to_string(a0 + 1) + "," + std::to_string(ac) + " +" + std::to_string(b0 + 1) +
           "," + std::to_string(bc) + " @@\n";
    for (size_t j = start; j < end; ++j) out += std::string(1, ops[j].op) + ops[j].text + "\n";
    i = end;
  }
  return out;
}

std::string own_text(const Store& s, const Hash& h) {
  // Children collapsed to a one-line placeholder so a class diff shows only its own text.
  Def d = load_def(s, h);
  std::string t;
  for (auto& p : d.parts) {
    if (p.type == Part::Text) t += p.text;
    else if (p.type == Part::Def) {
      Def c = load_def(s, p.hash);
      std::string first = render_def(s, p.hash);
      std::string indent_ws = first.substr(0, first.find_first_not_of(" \t"));
      t += indent_ws + "... " + c.kind + " " + c.name + " ...\n";
    } else {
      t += load_frag(s, p.hash).text;
    }
  }
  return t;
}

std::string module_text(const Store& s, const FileEntry& e) {
  if (e.hash.empty()) return "";
  if (e.kind == "blob") {
    std::string body = s.must_get(e.hash).body;
    bool binary = body.find('\0') != std::string::npos;
    if (binary) return "(binary file, " + std::to_string(body.size()) + " bytes)\n";
    return body;
  }
  std::string t;
  for (auto& p : load_file(s, e.hash).parts) {
    if (p.type == Part::Text) t += p.text;
    else if (p.type == Part::Def) {
      Def c = load_def(s, p.hash);
      t += "... " + c.kind + " " + c.name + " ...\n";
    } else {
      t += load_frag(s, p.hash).text;
    }
  }
  return t;
}

std::pair<std::string, std::string> change_texts(const Store& s, const DefChange& c) {
  return {c.old_hash.empty() ? "" : own_text(s, c.old_hash), c.new_hash.empty() ? "" : own_text(s, c.new_hash)};
}

std::pair<std::string, std::string> change_texts(const Store& s, const FileChange& c) {
  return {module_text(s, c.old_entry), module_text(s, c.new_entry)};
}

// Module code that is only definition placeholders and blank lines is not
// worth showing for an added or removed file.
static bool has_module_code(const std::string& t) {
  for (auto& l : split_lines(t)) {
    std::string x = trim(l);
    if (!x.empty() && !(starts_with(x, "... ") && x.size() > 7 && x.substr(x.size() - 4) == " ...")) return true;
  }
  return false;
}

NamespaceDiff diff_namespaces(const Store& s, const Namespace& a, const Namespace& b) {
  NamespaceDiff d;
  for (auto& [f, e] : b.files)
    if (!a.files.count(f)) {
      d.files_added.push_back(f);
      if (has_module_code(module_text(s, e))) d.module_changes.push_back({f, "added", {}, e});
    }
  for (auto& [f, e] : a.files)
    if (!b.files.count(f)) {
      d.files_removed.push_back(f);
      if (has_module_code(module_text(s, e))) d.module_changes.push_back({f, "removed", e, {}});
    }
  // Module-level glue per file.
  for (auto& [f, eb] : b.files) {
    auto it = a.files.find(f);
    if (it == a.files.end() || it->second.hash == eb.hash) continue;
    if (eb.kind != "file" || it->second.kind != "file") {
      d.files_glue_changed.push_back(f);
      d.module_changes.push_back({f, "changed", it->second, eb});
      continue;
    }
    // Module code without blank lines: spacing between definitions isn't a change.
    auto glue = [&](const Hash& h) {
      std::string g;
      for (auto& p : load_file(s, h).parts)
        if (p.type == Part::Text)
          for (auto& l : split_lines(p.text))
            if (!trim(l).empty()) g += l;
      return g;
    };
    if (glue(it->second.hash) != glue(eb.hash)) {
      d.files_glue_changed.push_back(f);
      d.module_changes.push_back({f, "changed", it->second, eb});
    }
  }

  auto fa = flatten(s, a), fb = flatten(s, b);
  std::map<std::string, const FlatDef*> by_lineage_a, by_lineage_b;
  for (auto& f : fa) by_lineage_a[f.lineage] = &f;
  for (auto& f : fb) by_lineage_b[f.lineage] = &f;
  // A container's own text: its text parts, with children reduced to names.
  auto own = [&](const FlatDef& f) {
    std::string t;
    for (auto& p : f.def.parts)
      t += p.type == Part::Text ? p.text : std::string("\x1f") + load_def(s, p.hash).name + "\x1f";
    return t;
  };
  for (auto& f : fb) {
    auto it = by_lineage_a.find(f.lineage);
    if (it == by_lineage_a.end()) {
      d.defs.push_back({"added", "", f.qname, "", f.hash, f.lineage});
      continue;
    }
    const FlatDef& o = *it->second;
    bool renamed = o.qname != f.qname;
    bool text_changed = own(o) != own(f);
    if (!renamed && !text_changed) continue;
    std::string kind = renamed ? "renamed" : (o.def.norm == f.def.norm ? "formatting" : "changed");
    if (renamed && text_changed && o.def.norm != f.def.norm) kind = "renamed+changed";
    d.defs.push_back({kind, o.qname, f.qname, o.hash, f.hash, f.lineage});
  }
  for (auto& f : fa)
    if (!by_lineage_b.count(f.lineage)) d.defs.push_back({"removed", f.qname, "", f.hash, "", f.lineage});
  d.tests_changed = a.tests != b.tests;
  return d;
}


static std::string prefixed(const std::string& text, char mark) {
  std::string out;
  for (auto& l : split_lines(text)) {
    std::string t = l;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    out += std::string(1, mark) + t + "\n";
  }
  return out;
}

nlohmann::json diff_to_json(const Store& s, const NamespaceDiff& d, bool with_text, int max_lines) {
  int used = 0, omitted = 0;
  // Adds text under key if it fits the budget; otherwise marks it omitted.
  auto put_text = [&](nlohmann::json& j, const char* key, const std::string& text) {
    int n = count_lines(text);
    if (max_lines > 0 && used + n > max_lines && used > 0) {
      j["omitted_lines"] = n;
      ++omitted;
      return;
    }
    used += n;
    j[key] = text;
  };
  nlohmann::json defs = nlohmann::json::array();
  for (auto& c : d.defs) {
    nlohmann::json j = {{"change", c.kind}};
    if (!c.old_name.empty()) j["old"] = c.old_name;
    if (!c.new_name.empty()) j["new"] = c.new_name;
    if (with_text) {
      auto [a, b] = change_texts(s, c);
      if (c.kind == "added") put_text(j, "text", b);
      else if (c.kind == "removed") put_text(j, "removed_text", a);
      else put_text(j, "diff", unified_diff(a, b));
    }
    defs.push_back(j);
  }
  nlohmann::json out = {{"definitions", defs},
                        {"files_added", d.files_added},
                        {"files_removed", d.files_removed},
                        {"module_code_changed", d.files_glue_changed},
                        {"test_links_changed", d.tests_changed}};
  if (with_text && !d.module_changes.empty()) {
    nlohmann::json mods = nlohmann::json::array();
    for (auto& m : d.module_changes) {
      nlohmann::json j = {{"file", m.path}, {"change", m.kind}};
      auto [a, b] = change_texts(s, m);
      if (m.kind == "added") put_text(j, "text", b);
      else if (m.kind == "removed") put_text(j, "removed_text", a);
      else put_text(j, "diff", unified_diff(a, b));
      mods.push_back(j);
    }
    out["module_code"] = mods;
  }
  if (omitted)
    out["truncated"] = std::to_string(omitted) + " change(s) listed without text to stay under " +
                       std::to_string(max_lines) + " lines; use hs_diff with a name to see them";
  return out;
}

std::string diff_to_text(const Store& s, const NamespaceDiff& d, bool with_text) {
  std::string out;
  for (auto& f : d.files_added) out += "  new file      " + f + "\n";
  for (auto& f : d.files_removed) out += "  deleted file  " + f + "\n";
  for (auto& c : d.defs) {
    std::string label = c.kind;
    label.resize(std::max<size_t>(label.size(), 14), ' ');
    std::string name = c.kind == "removed" ? c.old_name
                       : (c.kind == "renamed" || c.kind == "renamed+changed")
                           ? c.old_name + " -> " + c.new_name
                           : c.new_name;
    out += "  " + label + name + "\n";
    if (!with_text) continue;
    auto [a, b] = change_texts(s, c);
    std::string body = c.kind == "added" ? prefixed(b, '+') : c.kind == "removed" ? prefixed(a, '-') : unified_diff(a, b);
    for (auto& l : split_lines(body)) out += "      " + l;
  }
  if (with_text) {
    for (auto& m : d.module_changes) {
      if (m.kind != "changed") continue;
      out += "  module code   " + m.path + "\n";
      auto [a, b] = change_texts(s, m);
      for (auto& l : split_lines(unified_diff(a, b))) out += "      " + l;
    }
    for (auto& f : d.files_glue_changed) {
      bool shown = false;
      for (auto& m : d.module_changes) shown |= m.path == f;
      if (!shown) out += "  module code   " + f + "\n";
    }
  } else {
    for (auto& f : d.files_glue_changed) out += "  module code   " + f + "\n";
  }
  if (d.tests_changed) out += "  test links changed\n";
  return out;
}

}  // namespace hs
