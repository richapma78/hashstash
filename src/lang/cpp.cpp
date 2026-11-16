// C and C++ adapter (tree-sitter-cpp parses both). Definitions: functions
// (including out-of-class methods "Store::put"), classes/structs/unions/enums,
// typedefs of those, and namespaces as containers. Methods defined inside a
// class body become its children. Bodies written with a macro, as test
// frameworks do (GoogleTest TEST(Suite, Name), Catch2 / doctest
// TEST_CASE("name"), Boost BOOST_AUTO_TEST_CASE(name)), are functions named
// after the macro and its arguments: "TEST(Suite,Name)", "TEST_CASE(adds_up)". Preprocessor conditionals are looked
// through, so include guards and #ifdef blocks don't hide definitions; the
// directive lines themselves stay glue.
#include <tree_sitter/api.h>

#include <cstring>
#include <functional>

#include "core/util.h"
#include "lang/lang.h"

extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace hs {
namespace {

struct Tree {
  TSParser* parser;
  TSTree* tree;
  explicit Tree(std::string_view src) {
    parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_cpp());
    tree = ts_parser_parse_string(parser, nullptr, src.data(), (uint32_t)src.size());
  }
  ~Tree() {
    ts_tree_delete(tree);
    ts_parser_delete(parser);
  }
  TSNode root() const { return ts_tree_root_node(tree); }
};

bool is(TSNode n, const char* t) { return std::strcmp(ts_node_type(n), t) == 0; }
std::string_view text_of(TSNode n, std::string_view src) {
  return src.substr(ts_node_start_byte(n), ts_node_end_byte(n) - ts_node_start_byte(n));
}
TSNode field(TSNode n, const char* f) { return ts_node_child_by_field_name(n, f, (uint32_t)std::strlen(f)); }
bool is_preproc_block(TSNode n) {
  return is(n, "preproc_if") || is(n, "preproc_ifdef") || is(n, "preproc_else") || is(n, "preproc_elif") ||
         is(n, "preproc_elifdef");
}
bool is_record(TSNode n) {
  return is(n, "class_specifier") || is(n, "struct_specifier") || is(n, "union_specifier") ||
         is(n, "enum_specifier");
}

uint32_t last_row(TSNode n) {
  TSPoint s = ts_node_start_point(n), e = ts_node_end_point(n);
  if (e.column == 0 && e.row > s.row) return e.row - 1;
  return e.row;
}

// The function name from a declarator chain: pointer/reference wrappers, then
// function_declarator -> declarator.
std::string function_name(TSNode decl, std::string_view src) {
  while (!ts_node_is_null(decl)) {
    if (is(decl, "function_declarator")) {
      TSNode d = field(decl, "declarator");
      return ts_node_is_null(d) ? "<anonymous>" : collapse_ws(text_of(d, src));
    }
    TSNode inner = field(decl, "declarator");
    if (ts_node_is_null(inner)) break;
    decl = inner;
  }
  return "";
}

// An all-caps identifier such as TEST or BOOST_AUTO_TEST_CASE.
bool is_macro_name(std::string_view s) {
  bool letter = false;
  for (char c : s) {
    if (c >= 'A' && c <= 'Z') letter = true;
    else if (!(c >= '0' && c <= '9') && c != '_') return false;
  }
  return letter && s.size() >= 2;
}

// "TEST(Suite,Name)" from a macro and its argument list: identifiers as
// written, the first string literal's text (later ones are Catch2 tags), and
// characters a qualified name can't hold (. : ~ spaces quotes) as "_".
std::string macro_def_name(std::string_view macro, TSNode args, std::string_view src) {
  std::string name(macro);
  name += '(';
  bool first = true, had_string = false;
  uint32_t c = ts_node_named_child_count(args);
  for (uint32_t i = 0; i < c; ++i) {
    TSNode a = ts_node_named_child(args, i);
    if (is(a, "comment")) continue;
    std::string t;
    if (is(a, "string_literal") || is(a, "raw_string_literal")) {
      if (had_string) continue;
      had_string = true;
      std::string_view raw = text_of(a, src);
      size_t q1 = raw.find('"'), q2 = raw.rfind('"');
      t = q1 != std::string_view::npos && q2 > q1 ? std::string(raw.substr(q1 + 1, q2 - q1 - 1)) : std::string(raw);
    } else {
      t = collapse_ws(text_of(a, src));
    }
    for (char& ch : t)
      if (ch == '.' || ch == ':' || ch == '~' || ch == ' ' || ch == '\t' || ch == '"' || ch == '\'') ch = '_';
    name += (first ? "" : ",") + t;
    first = false;
  }
  return name + ')';
}

// A macro call written as a body's head: "TEST_CASE("x") { ... }" parses as
// a call statement (missing its ";") followed by a block. The macro's name,
// or "" if st isn't one.
std::string macro_call_head(TSNode st, std::string_view src, TSNode* args) {
  if (!is(st, "expression_statement") || ts_node_named_child_count(st) != 1) return "";
  TSNode call = ts_node_named_child(st, 0);
  if (!is(call, "call_expression")) return "";
  TSNode fn = field(call, "function");
  if (ts_node_is_null(fn) || !is(fn, "identifier") || !is_macro_name(text_of(fn, src))) return "";
  *args = field(call, "arguments");
  return ts_node_is_null(*args) ? "" : std::string(text_of(fn, src));
}

struct Found {
  std::string kind, name;
  TSNode body{};  // container body to descend into (class field list / namespace body)
};

// Is n (a statement in a block) a definition? Fills kind/name/body.
bool definition(TSNode n, std::string_view src, bool in_class, Found& f) {
  if (is(n, "template_declaration")) {
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode k = ts_node_named_child(n, i);
      if (is(k, "template_parameter_list")) continue;
      if (definition(k, src, in_class, f)) return true;
    }
    return false;
  }
  if (is(n, "function_definition")) {
    f.name = function_name(field(n, "declarator"), src);
    if (f.name.empty()) return false;
    // TEST(Suite, Name) { ... }: no return type, an all-caps "function".
    TSNode d = field(n, "declarator");
    if (!in_class && ts_node_is_null(field(n, "type")) && is(d, "function_declarator") &&
        is_macro_name(f.name) && !ts_node_is_null(field(d, "parameters"))) {
      f.name = macro_def_name(f.name, field(d, "parameters"), src);
      f.kind = "function";
      return true;
    }
    f.kind = in_class || f.name.find("::") != std::string::npos ? "method" : "function";
    return true;
  }
  if (is(n, "namespace_definition")) {
    TSNode nm = field(n, "name");
    f.kind = "namespace";
    f.name = ts_node_is_null(nm) ? "<anonymous>" : collapse_ws(text_of(nm, src));
    f.body = field(n, "body");
    return true;
  }
  TSNode rec{};
  if (is_record(n)) rec = n;
  else if (is(n, "declaration") || is(n, "field_declaration") || is(n, "type_definition")) {
    TSNode t = field(n, "type");
    if (!ts_node_is_null(t) && is_record(t)) rec = t;
  }
  if (ts_node_is_null(rec) || ts_node_is_null(field(rec, "body"))) return false;
  TSNode nm = field(rec, "name");
  if (is(n, "type_definition")) {
    TSNode d = field(n, "declarator");
    if (!ts_node_is_null(d)) nm = d;
  }
  if (ts_node_is_null(nm)) return false;  // anonymous struct: leave as glue
  f.name = collapse_ws(text_of(nm, src));
  std::string t = ts_node_type(rec);
  f.kind = t.substr(0, t.find('_'));  // class / struct / union / enum
  if (f.kind != "enum") f.body = field(rec, "body");
  return true;
}

class Cpp : public Language {
 public:
  std::string name() const override { return "cpp"; }
  bool handles(const std::string& path) const override {
    for (const char* ext : {".c", ".h", ".cc", ".cpp", ".cxx", ".c++", ".hpp", ".hh", ".hxx", ".h++", ".ipp", ".inl", ".tpp"}) {
      size_t n = std::strlen(ext);
      if (path.size() > n && path.compare(path.size() - n, n, ext) == 0) return true;
    }
    return false;
  }
  std::string comment_prefix() const override { return "//"; }

  std::vector<Chunk> split(std::string_view src) const override {
    Tree t(src);
    auto starts = line_starts(src);
    auto line_begin = [&](uint32_t row) { return row < starts.size() ? starts[row] : src.size(); };
    auto line_end = [&](uint32_t row) { return row + 1 < starts.size() ? starts[row + 1] : src.size(); };

    // Statements of a block, looking through preprocessor conditionals.
    std::function<void(TSNode, std::vector<TSNode>&)> items = [&](TSNode block, std::vector<TSNode>& out) {
      uint32_t c = ts_node_named_child_count(block);
      for (uint32_t i = 0; i < c; ++i) {
        TSNode k = ts_node_named_child(block, i);
        if (is_preproc_block(k)) {
          // Keep the directive's own pieces (condition, name) out of the list.
          uint32_t kc = ts_node_named_child_count(k);
          for (uint32_t j = 0; j < kc; ++j) {
            TSNode g = ts_node_named_child(k, j);
            const char* fname = ts_node_field_name_for_named_child(k, j);
            if (fname && (std::strcmp(fname, "condition") == 0 || std::strcmp(fname, "name") == 0)) continue;
            if (is_preproc_block(g)) {
              std::vector<TSNode> sub;
              items(g, sub);
              out.insert(out.end(), sub.begin(), sub.end());
            } else {
              out.push_back(g);
            }
          }
        } else {
          out.push_back(k);
        }
      }
    };

    std::function<std::vector<Chunk>(TSNode, bool)> split_block = [&](TSNode block, bool in_class) {
      std::vector<TSNode> stmts;
      items(block, stmts);
      std::vector<Chunk> out;
      long long prev_end = -1;
      for (size_t i = 0; i < stmts.size(); ++i) {
        TSNode st = stmts[i];
        size_t head = i;  // first statement of the definition (a macro body spans two)
        Found f;
        uint32_t first = ts_node_start_point(st).row, last = last_row(st);
        TSNode margs{};
        std::string macro = in_class ? "" : macro_call_head(st, src, &margs);
        bool macro_body = !macro.empty() && i + 1 < stmts.size() && is(stmts[i + 1], "compound_statement") &&
                          ts_node_start_point(stmts[i + 1]).row <= last + 1;
        if (macro_body) {
          f.kind = "function";
          f.name = macro_def_name(macro, margs, src);
          last = last_row(stmts[++i]);  // the block is part of it
        } else if (!definition(st, src, in_class, f)) {
          if (!is(st, "comment")) prev_end = std::max<long long>(prev_end, last);
          continue;
        }
        // A trailing ";" or comment on the last line belongs with it; anything
        // else sharing its first line means leave it as glue.
        if ((long long)first <= prev_end) {
          prev_end = std::max<long long>(prev_end, last);
          continue;
        }
        if (i + 1 < stmts.size() && ts_node_start_point(stmts[i + 1]).row == last &&
            !is(stmts[i + 1], "comment")) {
          prev_end = std::max<long long>(prev_end, last);
          continue;
        }
        uint32_t top = first;
        for (long long j = (long long)head - 1; j >= 0; --j) {
          TSNode c = stmts[(size_t)j];
          if (!is(c, "comment")) break;
          uint32_t crow = ts_node_start_point(c).row;
          if (last_row(c) + 1 != top || (long long)crow <= prev_end) break;
          std::string_view before = src.substr(line_begin(crow), ts_node_start_byte(c) - line_begin(crow));
          if (before.find_first_not_of(" \t") != std::string_view::npos) break;
          top = crow;
        }
        Chunk ch;
        ch.kind = f.kind;
        ch.name = f.name;
        ch.begin = line_begin(top);
        ch.end = line_end(last);
        if (!ts_node_is_null(f.body) && ts_node_start_point(f.body).row < last)
          ch.children = split_block(f.body, f.kind != "namespace");
        out.push_back(std::move(ch));
        prev_end = last;
      }
      return out;
    };
    return split_block(t.root(), false);
  }

  Analysis analyze(std::string_view text) const override {
    Tree t(text);
    Analysis a;
    std::string tokens;
    bool sig_done = false;
    std::function<void(TSNode)> visit = [&](TSNode n) {
      if (is(n, "comment")) return;
      uint32_t count = ts_node_child_count(n);
      if (count == 0 || is(n, "string_literal") || is(n, "raw_string_literal") || is(n, "char_literal")) {
        tokens += text_of(n, text);
        tokens += '\x1f';
      } else {
        for (uint32_t i = 0; i < count; ++i) visit(ts_node_child(n, i));
      }
      if (is(n, "call_expression")) {
        TSNode fn = field(n, "function");
        if (!ts_node_is_null(fn)) {
          if (is(fn, "template_function")) fn = field(fn, "name");
          if (!ts_node_is_null(fn) && (is(fn, "identifier") || is(fn, "qualified_identifier") ||
                                       is(fn, "field_expression"))) {
            std::string name = collapse_ws(text_of(fn, text));
            size_t arrow;
            while ((arrow = name.find("->")) != std::string::npos) name.replace(arrow, 2, ".");
            a.calls.push_back({name, (int)ts_node_start_point(n).row + 1});
          }
        }
      }
      if (is(n, "preproc_include")) {
        TSNode p = field(n, "path");
        if (!ts_node_is_null(p)) {
          std::string m(text_of(p, text));
          if (m.size() >= 2) m = m.substr(1, m.size() - 2);
          a.imports.push_back({m, "", "", 0});
        }
      }
      if (!sig_done && is(n, "function_definition")) {
        TSNode body = field(n, "body");
        size_t s = ts_node_start_byte(n), e = ts_node_is_null(body) ? ts_node_end_byte(n) : ts_node_start_byte(body);
        a.signature = collapse_ws(text.substr(s, e - s));
        sig_done = true;
      }
    };
    visit(t.root());
    a.norm = hash_bytes("norm", tokens);
    if (!sig_done) {  // a macro body: its head, 'TEST_CASE("adds {1}")'
      uint32_t c = ts_node_named_child_count(t.root());
      for (uint32_t i = 0; i < c; ++i) {
        TSNode k = ts_node_named_child(t.root(), i);
        if (is(k, "comment")) continue;
        TSNode args{};
        if (!macro_call_head(k, text, &args).empty()) {
          a.signature = collapse_ws(text_of(ts_node_named_child(k, 0), text));
          sig_done = true;
        }
        break;
      }
    }
    if (!sig_done) {
      // Records: "class Foo : public Bar"
      size_t brace = text.find('{');
      std::string head = collapse_ws(text.substr(0, brace == std::string_view::npos ? std::min<size_t>(text.size(), 200) : brace));
      // Skip leading comment lines.
      while (starts_with(head, "//") || starts_with(head, "/*")) {
        size_t nl = text.find('\n');
        if (nl == std::string_view::npos) break;
        text = text.substr(nl + 1);
        brace = text.find('{');
        head = collapse_ws(text.substr(0, brace == std::string_view::npos ? std::min<size_t>(text.size(), 200) : brace));
      }
      a.signature = head.substr(0, 200);
    }
    return a;
  }
};

}  // namespace

std::unique_ptr<Language> make_cpp() { return std::make_unique<Cpp>(); }

}  // namespace hs
