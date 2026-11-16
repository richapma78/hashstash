// Godot adapters.
//
// GDScript (.gd): each script is a class, so its funcs become definitions
// ("function" at the top, "method" inside an inner `class`), plus inner
// classes, the constructor (_init), properties with set/get code, and enums.
// Comment and @annotation lines directly above a definition belong to it.
// extends / class_name / signals / constants / plain vars stay module code.
//
// Text scenes and resources (.tscn, .tres, .escn) and project.godot: each
// section becomes a definition, so a change to one node, resource or signal
// connection shows as a change to that item alone:
//   [node name="Shape" parent="Hurtbox"]       node   "Hurtbox/Shape" (path from the root)
//   [ext_resource ... id="1_abc"]              ext_resource "ext_1_abc"
//   [sub_resource type="X" id="X_q2w"]         sub_resource "X_q2w"
//   [resource]                                 resource "resource"
//   [connection signal="s" from="F" method="m"] connection "F/s-m" ("s-m" from the root)
//   [application] (project.godot)              section "application"
// The file header ([gd_scene ...] / [gd_resource ...]) and blank lines stay
// module code. References (ExtResource("id"), SubResource("id"), connection
// methods) are recorded as calls, so the graph shows what uses what.
#include <tree_sitter/api.h>

#include <cstring>
#include <functional>
#include <map>

#include "lang/lang.h"

extern "C" const TSLanguage* tree_sitter_gdscript(void);

namespace hs {
namespace {

bool ends_with_ext(const std::string& path, const char* ext) {
  size_t n = std::strlen(ext);
  return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
}

// ------------------------------------------------------------------ GDScript

struct GdTree {
  TSParser* parser;
  TSTree* tree;
  explicit GdTree(std::string_view src) {
    parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_gdscript());
    tree = ts_parser_parse_string(parser, nullptr, src.data(), (uint32_t)src.size());
  }
  ~GdTree() {
    ts_tree_delete(tree);
    ts_parser_delete(parser);
  }
  TSNode root() const { return ts_tree_root_node(tree); }
};

bool is_type(TSNode n, const char* t) { return std::strcmp(ts_node_type(n), t) == 0; }

std::string_view node_text(TSNode n, std::string_view src) {
  return src.substr(ts_node_start_byte(n), ts_node_end_byte(n) - ts_node_start_byte(n));
}

uint32_t last_row(TSNode n) {
  TSPoint s = ts_node_start_point(n), e = ts_node_end_point(n);
  if (e.column == 0 && e.row > s.row) return e.row - 1;
  return e.row;
}

TSNode named_child_of_type(TSNode n, const char* type) {
  uint32_t c = ts_node_named_child_count(n);
  for (uint32_t i = 0; i < c; ++i)
    if (is_type(ts_node_named_child(n, i), type)) return ts_node_named_child(n, i);
  return TSNode{};
}

// What kind of definition a statement is ("" if it stays module code).
std::string gd_kind(TSNode n, bool in_class) {
  if (is_type(n, "function_definition") || is_type(n, "constructor_definition")) return in_class ? "method" : "function";
  if (is_type(n, "class_definition")) return "class";
  if (is_type(n, "enum_definition")) return "enum";
  if (is_type(n, "variable_statement") && !ts_node_is_null(ts_node_child_by_field_name(n, "setget", 6))) return "property";
  return "";
}

std::string gd_name(TSNode n, std::string_view src) {
  if (is_type(n, "constructor_definition")) return "_init";
  TSNode name = ts_node_child_by_field_name(n, "name", 4);
  if (ts_node_is_null(name)) return "<anonymous>";
  return std::string(node_text(name, src));
}

TSNode gd_body(TSNode n) {
  TSNode b = ts_node_child_by_field_name(n, "body", 4);
  if (ts_node_is_null(b)) b = named_child_of_type(n, "class_body");
  if (ts_node_is_null(b)) b = named_child_of_type(n, "body");
  return b;
}

class GdScript : public Language {
 public:
  std::string name() const override { return "gdscript"; }
  bool handles(const std::string& path) const override { return ends_with_ext(path, ".gd"); }
  std::string comment_prefix() const override { return "#"; }

  std::vector<Chunk> split(std::string_view src) const override {
    GdTree t(src);
    auto starts = line_starts(src);
    auto line_begin = [&](uint32_t row) { return row < starts.size() ? starts[row] : src.size(); };
    auto line_end = [&](uint32_t row) { return row + 1 < starts.size() ? starts[row + 1] : src.size(); };

    std::function<std::vector<Chunk>(TSNode, bool)> split_block = [&](TSNode block, bool in_class) {
      std::vector<Chunk> out;
      uint32_t n = ts_node_named_child_count(block);
      long long prev_end_row = -1;
      for (uint32_t i = 0; i < n; ++i) {
        TSNode stmt = ts_node_named_child(block, i);
        std::string kind = gd_kind(stmt, in_class);
        uint32_t first = ts_node_start_point(stmt).row, last = last_row(stmt);
        if (kind.empty()) {
          // Comments and annotations only matter as attachments below.
          if (!is_type(stmt, "comment") && !is_type(stmt, "annotation"))
            prev_end_row = std::max<long long>(prev_end_row, last);
          continue;
        }
        if ((long long)first <= prev_end_row) {  // shares a line with something else
          prev_end_row = std::max<long long>(prev_end_row, last);
          continue;
        }
        // Comment and annotation lines directly above, each starting its line.
        uint32_t top = first;
        for (long long j = (long long)i - 1; j >= 0; --j) {
          TSNode c = ts_node_named_child(block, (uint32_t)j);
          if (!is_type(c, "comment") && !is_type(c, "annotation")) break;
          uint32_t crow = ts_node_start_point(c).row;
          if (last_row(c) + 1 != top || (long long)crow <= prev_end_row) break;
          std::string_view before = src.substr(line_begin(crow), ts_node_start_byte(c) - line_begin(crow));
          if (before.find_first_not_of(" \t") != std::string_view::npos) break;
          top = crow;
        }
        // A file's top annotation (@tool) belongs to the file, not the first definition.
        if (top == 0 && !in_class && first != 0) {
          TSNode c0 = ts_node_named_child(block, 0);
          if (is_type(c0, "annotation") && ts_node_start_point(c0).row == 0) top = 1;
        }
        Chunk ch;
        ch.kind = kind;
        ch.name = gd_name(stmt, src);
        ch.begin = line_begin(top);
        ch.end = line_end(last);
        if (kind == "class") {
          TSNode body = gd_body(stmt);
          if (!ts_node_is_null(body) && ts_node_start_point(body).row > ts_node_start_point(stmt).row)
            ch.children = split_block(body, true);
          else if (!ts_node_is_null(body) && last > first)
            ch.children = split_block(body, true);
        }
        out.push_back(std::move(ch));
        prev_end_row = last;
      }
      return out;
    };
    return split_block(t.root(), false);
  }

  Analysis analyze(std::string_view raw) const override {
    std::string text = dedent(raw);
    GdTree t(text);
    Analysis a;
    std::string tokens;
    std::function<void(TSNode)> visit = [&](TSNode n) {
      if (is_type(n, "comment")) return;
      uint32_t count = ts_node_child_count(n);
      bool block = is_type(n, "body") || is_type(n, "class_body");
      if (block) tokens += "{\x1f";
      if (count == 0 || is_type(n, "string")) {
        tokens += node_text(n, text);
        tokens += '\x1f';
      } else {
        for (uint32_t i = 0; i < count; ++i) visit(ts_node_child(n, i));
      }
      if (block) tokens += "}\x1f";
      int line = (int)ts_node_start_point(n).row + 1;
      if (is_type(n, "call") && ts_node_named_child_count(n) > 0) {
        TSNode fn = ts_node_named_child(n, 0);
        if (is_type(fn, "identifier")) a.calls.push_back({std::string(node_text(fn, text)), line});
      } else if (is_type(n, "attribute")) {
        // a.b.c(...) is attribute(identifier a, identifier b, attribute_call(c, args))
        std::string chain;
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) {
          TSNode k = ts_node_named_child(n, i);
          if (is_type(k, "identifier")) {
            chain += (chain.empty() ? "" : ".") + std::string(node_text(k, text));
          } else if (is_type(k, "attribute_call") && ts_node_named_child_count(k) > 0) {
            chain += (chain.empty() ? "" : ".") + std::string(node_text(ts_node_named_child(k, 0), text));
            a.calls.push_back({chain, line});
          } else {
            break;
          }
        }
      } else if (is_type(n, "base_call") && ts_node_named_child_count(n) > 0) {
        a.calls.push_back({"super." + std::string(node_text(ts_node_named_child(n, 0), text)), line});
      }
    };
    visit(t.root());
    a.norm = hash_bytes("norm", tokens);

    // Signature and docs (## comment lines above, or a leading string) of the first definition.
    uint32_t n = ts_node_named_child_count(t.root());
    std::string doc;
    for (uint32_t i = 0; i < n; ++i) {
      TSNode def = ts_node_named_child(t.root(), i);
      if (is_type(def, "comment")) {
        std::string c(node_text(def, text));
        if (c.rfind("##", 0) == 0) doc += (doc.empty() ? "" : " ") + collapse_ws(c.substr(2));
        continue;
      }
      if (gd_kind(def, false).empty()) continue;
      TSNode body = gd_body(def);
      if (is_type(def, "variable_statement")) body = ts_node_child_by_field_name(def, "setget", 6);
      size_t s = ts_node_start_byte(def);
      size_t e = ts_node_is_null(body) ? ts_node_end_byte(def) : ts_node_start_byte(body);
      std::string sig = collapse_ws(std::string_view(text).substr(s, e - s));
      while (!sig.empty() && (sig.back() == ':' || sig.back() == ' ')) sig.pop_back();
      a.signature = sig;
      if (doc.empty() && !ts_node_is_null(body) && ts_node_named_child_count(body) > 0) {
        TSNode first = ts_node_named_child(body, 0);
        if (is_type(first, "expression_statement") && ts_node_named_child_count(first) > 0 &&
            is_type(ts_node_named_child(first, 0), "string")) {
          std::string d(node_text(ts_node_named_child(first, 0), text));
          size_t open = d.find_first_not_of("\"'"), close = d.find_last_not_of("\"'");
          if (open != std::string::npos && close != std::string::npos && close >= open) d = d.substr(open, close - open + 1);
          doc = collapse_ws(d);
        }
      }
      break;
    }
    a.doc = doc.substr(0, 400);
    return a;
  }
};

// ----------------------------------------------- scenes, resources, project

// key="value" pairs of a section header line.
std::map<std::string, std::string> header_attrs(std::string_view line) {
  std::map<std::string, std::string> out;
  size_t i = line.find(' ');
  while (i != std::string_view::npos && i < line.size()) {
    while (i < line.size() && line[i] == ' ') ++i;
    size_t eq = line.find('=', i);
    if (eq == std::string_view::npos) break;
    std::string key(line.substr(i, eq - i));
    size_t v = eq + 1;
    std::string val;
    if (v < line.size() && line[v] == '"') {
      size_t j = v + 1;
      for (; j < line.size() && line[j] != '"'; ++j) {
        if (line[j] == '\\' && j + 1 < line.size()) ++j;
        val += line[j];
      }
      i = j + 1;
    } else {
      size_t j = v;
      while (j < line.size() && line[j] != ' ' && line[j] != ']') val += line[j++];
      i = j;
    }
    out[key] = val;
  }
  return out;
}

class GodotResource : public Language {
 public:
  std::string name() const override { return "godot"; }
  bool handles(const std::string& path) const override {
    return ends_with_ext(path, ".tscn") || ends_with_ext(path, ".tres") || ends_with_ext(path, ".escn") ||
           path == "project.godot" || ends_with_ext(path, "/project.godot");
  }
  std::string comment_prefix() const override { return ";"; }

  std::vector<Chunk> split(std::string_view src) const override {
    struct Sec {
      size_t line;
      std::string tag;
      std::map<std::string, std::string> attrs;
    };
    auto starts = line_starts(src);
    auto line_at = [&](size_t i) {
      size_t b = starts[i], e = i + 1 < starts.size() ? starts[i + 1] : src.size();
      std::string_view l = src.substr(b, e - b);
      while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.remove_suffix(1);
      return l;
    };
    // Section headers: a line starting with "[" outside any string value.
    std::vector<Sec> secs;
    bool in_string = false;
    for (size_t i = 0; i < starts.size(); ++i) {
      std::string_view l = line_at(i);
      if (!in_string && !l.empty() && l[0] == '[' && l.back() == ']') {
        size_t sp = l.find_first_of(" ]");
        secs.push_back({i, std::string(l.substr(1, sp - 1)), header_attrs(l)});
      }
      for (size_t k = 0; k < l.size(); ++k) {
        if (in_string && l[k] == '\\') ++k;
        else if (l[k] == '"') in_string = !in_string;
      }
    }
    std::vector<Chunk> out;
    for (size_t s = 0; s < secs.size(); ++s) {
      const Sec& sec = secs[s];
      Chunk ch;
      const std::string& t = sec.tag;
      auto attr = [&](const char* k) {
        auto it = sec.attrs.find(k);
        return it == sec.attrs.end() ? std::string() : it->second;
      };
      if (t == "gd_scene" || t == "gd_resource") continue;  // file header: module code
      if (t == "node") {
        // The node's path from the scene root, as get_node() takes it: the root
        // is its own name, its children "Sprite2D", grandchildren "Hurtbox/Shape".
        // Taken from the header alone, so a node edited on its own keeps its name.
        std::string parent = attr("parent"), nm = attr("name");
        ch.kind = "node";
        ch.name = !sec.attrs.count("parent") || parent == "." ? nm : parent + "/" + nm;
      } else if (t == "ext_resource") {
        ch.kind = "ext_resource";
        ch.name = "ext_" + attr("id");
      } else if (t == "sub_resource") {
        ch.kind = "sub_resource";
        ch.name = attr("id");
      } else if (t == "resource") {
        ch.kind = "resource";
        ch.name = "resource";
      } else if (t == "connection") {
        // "<from>/<signal>-<method>"; from the root: "<signal>-<method>"
        ch.kind = "connection";
        std::string from = attr("from");
        ch.name = (from.empty() || from == "." ? "" : from + "/") + attr("signal") + "-" + attr("method");
      } else if (t == "editable") {
        continue;
      } else {
        ch.kind = "section";  // project.godot and other config files
        ch.name = t;
      }
      for (char& c : ch.name)
        if (c == ':' || c == '.' || c == ' ' || c == '"') c = '_';
      if (ch.name.empty() || ch.name == "_") ch.name = ch.kind;
      // The section runs to the last non-blank line before the next header.
      size_t endl = s + 1 < secs.size() ? secs[s + 1].line : starts.size();
      size_t last = sec.line;
      for (size_t i = sec.line; i < endl; ++i)
        if (line_at(i).find_first_not_of(" \t") != std::string_view::npos) last = i;
      ch.begin = starts[sec.line];
      ch.end = last + 1 < starts.size() ? starts[last + 1] : src.size();
      out.push_back(std::move(ch));
    }
    return out;
  }

  Analysis analyze(std::string_view text) const override {
    Analysis a;
    // Normalized: each line with runs of whitespace collapsed, blank lines dropped.
    std::string tokens;
    size_t i = 0;
    int line = 0;
    while (i < text.size()) {
      size_t eol = text.find('\n', i);
      if (eol == std::string_view::npos) eol = text.size();
      std::string_view l = text.substr(i, eol - i);
      ++line;
      std::string c = collapse_ws(l);
      if (!c.empty()) {
        if (a.signature.empty() && c[0] == '[') a.signature = c;
        tokens += c + '\x1f';
      }
      // References to other items in the same file.
      for (const char* fn : {"ExtResource(\"", "SubResource(\""}) {
        size_t at = 0;
        while ((at = l.find(fn, at)) != std::string_view::npos) {
          size_t s = at + std::strlen(fn), e = l.find('"', s);
          if (e == std::string_view::npos) break;
          std::string id(l.substr(s, e - s));
          a.calls.push_back({fn[0] == 'E' ? "ext_" + id : id, line});
          at = e;
        }
      }
      if (c.rfind("[connection", 0) == 0) {
        auto attrs = header_attrs(c);
        if (attrs.count("method")) a.calls.push_back({attrs["method"], line});
      }
      i = eol + 1;
    }
    a.norm = hash_bytes("norm", tokens);
    return a;
  }
};

}  // namespace

std::unique_ptr<Language> make_gdscript() { return std::make_unique<GdScript>(); }
std::unique_ptr<Language> make_godot_resource() { return std::make_unique<GodotResource>(); }

}  // namespace hs
