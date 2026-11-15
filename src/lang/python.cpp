// Python adapter: top-level functions and classes become definitions, and
// methods / nested classes inside a class body become child definitions.
// Comment lines directly above a definition (no blank line between) belong to it.
#include <tree_sitter/api.h>

#include <cstring>
#include <functional>

#include "lang/lang.h"

extern "C" const TSLanguage* tree_sitter_python(void);

namespace hs {
namespace {

struct Tree {
  TSParser* parser;
  TSTree* tree;
  Tree(std::string_view src) {
    parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_python());
    tree = ts_parser_parse_string(parser, nullptr, src.data(), (uint32_t)src.size());
  }
  ~Tree() {
    ts_tree_delete(tree);
    ts_parser_delete(parser);
  }
  TSNode root() const { return ts_tree_root_node(tree); }
};

bool is_type(TSNode n, const char* t) { return std::strcmp(ts_node_type(n), t) == 0; }

std::string_view node_text(TSNode n, std::string_view src) {
  return src.substr(ts_node_start_byte(n), ts_node_end_byte(n) - ts_node_start_byte(n));
}

// The function/class node behind a definition statement, or null.
TSNode definition_of(TSNode n) {
  if (is_type(n, "function_definition") || is_type(n, "class_definition")) return n;
  if (is_type(n, "decorated_definition")) return ts_node_child_by_field_name(n, "definition", 10);
  return TSNode{};
}

uint32_t last_row(TSNode n) {
  TSPoint s = ts_node_start_point(n), e = ts_node_end_point(n);
  if (e.column == 0 && e.row > s.row) return e.row - 1;
  return e.row;
}

void collect_import(TSNode n, std::string_view src, Analysis& a) {
  bool from = is_type(n, "import_from_statement");
  Import base;
  if (from) {
    TSNode mod = ts_node_child_by_field_name(n, "module_name", 11);
    if (!ts_node_is_null(mod)) {
      std::string m(node_text(mod, src));
      size_t dots = 0;
      while (dots < m.size() && m[dots] == '.') ++dots;
      base.level = (int)dots;
      base.module = collapse_ws(m.substr(dots));
    }
  }
  uint32_t count = ts_node_child_count(n);
  for (uint32_t i = 0; i < count; ++i) {
    const char* field = ts_node_field_name_for_child(n, i);
    if (!field || std::strcmp(field, "name") != 0) continue;
    TSNode c = ts_node_child(n, i);
    Import im = base;
    std::string target, alias;
    if (is_type(c, "aliased_import")) {
      TSNode nm = ts_node_child_by_field_name(c, "name", 4);
      TSNode al = ts_node_child_by_field_name(c, "alias", 5);
      if (!ts_node_is_null(nm)) target = node_text(nm, src);
      if (!ts_node_is_null(al)) alias = node_text(al, src);
    } else {
      target = node_text(c, src);
    }
    if (from) im.name = collapse_ws(target);
    else im.module = collapse_ws(target);
    im.alias = alias;
    a.imports.push_back(im);
  }
  if (from) {  // "from x import *"
    for (uint32_t i = 0; i < count; ++i)
      if (is_type(ts_node_child(n, i), "wildcard_import")) {
        Import im = base;
        im.name = "*";
        a.imports.push_back(im);
      }
  }
}

class Python : public Language {
 public:
  std::string name() const override { return "python"; }
  bool handles(const std::string& path) const override {
    auto ends = [&](const char* ext) {
      size_t n = std::strlen(ext);
      return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
    };
    return ends(".py") || ends(".pyi");
  }
  std::string comment_prefix() const override { return "#"; }

  std::vector<Chunk> split(std::string_view src) const override {
    Tree t(src);
    auto starts = line_starts(src);
    auto line_begin = [&](uint32_t row) { return row < starts.size() ? starts[row] : src.size(); };
    auto line_end = [&](uint32_t row) {
      return row + 1 < starts.size() ? starts[row + 1] : src.size();
    };

    // Splits the statements of one block (module or class body) into chunks.
    std::function<std::vector<Chunk>(TSNode, bool)> split_block = [&](TSNode block,
                                                                       bool in_class) {
      std::vector<Chunk> out;
      uint32_t n = ts_node_named_child_count(block);
      long long prev_end_row = -1;  // last row used by a previous chunk or statement
      for (uint32_t i = 0; i < n; ++i) {
        TSNode stmt = ts_node_named_child(block, i);
        TSNode def = definition_of(stmt);
        uint32_t first = ts_node_start_point(stmt).row;
        uint32_t last = last_row(stmt);
        if (ts_node_is_null(def)) {
          // Comments are free-floating; they only matter as attachments below.
          if (!is_type(stmt, "comment")) prev_end_row = std::max<long long>(prev_end_row, last);
          continue;
        }
        // Something else already on this definition's first line: leave it as glue.
        if ((long long)first <= prev_end_row) {
          prev_end_row = std::max<long long>(prev_end_row, last);
          continue;
        }
        // Attach comment lines directly above, each alone on its own line.
        uint32_t top = first;
        for (long long j = (long long)i - 1; j >= 0; --j) {
          TSNode c = ts_node_named_child(block, (uint32_t)j);
          if (!is_type(c, "comment")) break;
          uint32_t crow = ts_node_start_point(c).row;
          if (crow + 1 != top || (long long)crow <= prev_end_row) break;
          // The comment must start its line (only whitespace before it).
          std::string_view before = src.substr(line_begin(crow), ts_node_start_byte(c) - line_begin(crow));
          if (before.find_first_not_of(" \t") != std::string_view::npos) break;
          top = crow;
        }
        Chunk ch;
        bool is_class = is_type(def, "class_definition");
        ch.kind = is_class ? "class" : (in_class ? "method" : "function");
        TSNode name = ts_node_child_by_field_name(def, "name", 4);
        ch.name = ts_node_is_null(name) ? "<anonymous>" : std::string(node_text(name, src));
        ch.begin = line_begin(top);
        ch.end = line_end(last);
        if (is_class) {
          TSNode body = ts_node_child_by_field_name(def, "body", 4);
          if (!ts_node_is_null(body) && ts_node_start_point(body).row > ts_node_start_point(def).row)
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
    Tree t(text);
    Analysis a;

    // Normalized token stream: leaves without comments, with block markers so
    // indentation structure still counts.
    std::string tokens;
    std::function<void(TSNode)> visit = [&](TSNode n) {
      if (is_type(n, "comment")) return;
      uint32_t count = ts_node_child_count(n);
      bool block = is_type(n, "block");
      if (block) tokens += "{\x1f";
      if (count == 0 || is_type(n, "string")) {
        tokens += node_text(n, text);
        tokens += '\x1f';
      } else {
        for (uint32_t i = 0; i < count; ++i) visit(ts_node_child(n, i));
      }
      if (block) tokens += "}\x1f";
      if (is_type(n, "import_statement") || is_type(n, "import_from_statement")) collect_import(n, text, a);
      if (is_type(n, "call")) {
        TSNode fn = ts_node_child_by_field_name(n, "function", 8);
        if (!ts_node_is_null(fn) && (is_type(fn, "identifier") || is_type(fn, "attribute"))) {
          a.calls.push_back({collapse_ws(node_text(fn, text)), (int)ts_node_start_point(n).row + 1});
        }
      }
    };
    visit(t.root());
    a.norm = hash_bytes("norm", tokens);

    // Signature and docstring of the first definition in the text.
    uint32_t n = ts_node_named_child_count(t.root());
    for (uint32_t i = 0; i < n; ++i) {
      TSNode def = definition_of(ts_node_named_child(t.root(), i));
      if (ts_node_is_null(def)) continue;
      TSNode body = ts_node_child_by_field_name(def, "body", 4);
      size_t s = ts_node_start_byte(def);
      size_t e = ts_node_is_null(body) ? ts_node_end_byte(def) : ts_node_start_byte(body);
      std::string sig = collapse_ws(std::string_view(text).substr(s, e - s));
      while (!sig.empty() && (sig.back() == ':' || sig.back() == ' ')) sig.pop_back();
      a.signature = sig;
      if (!ts_node_is_null(body) && ts_node_named_child_count(body) > 0) {
        TSNode first = ts_node_named_child(body, 0);
        if (is_type(first, "expression_statement") && ts_node_named_child_count(first) > 0) {
          TSNode str = ts_node_named_child(first, 0);
          if (is_type(str, "string")) {
            std::string d(node_text(str, text));
            size_t q = d.find_first_of("\"'");
            if (q != std::string::npos) {
              char quote = d[q];
              size_t open = d.find_first_not_of(quote, q);
              size_t close = d.find_last_not_of(quote);
              if (open != std::string::npos && close != std::string::npos && close >= open)
                d = d.substr(open, close - open + 1);
            }
            a.doc = collapse_ws(d).substr(0, 400);
          }
        }
      }
      break;
    }
    return a;
  }
};

}  // namespace

std::unique_ptr<Language> make_python() { return std::make_unique<Python>(); }

}  // namespace hs
