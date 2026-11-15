#include "ops/build.h"

#include "core/util.h"

namespace hs {

std::string own_text(const Def& d) {
  std::string t;
  for (auto& p : d.parts)
    if (p.type == Part::Text) t += p.text;
  return t;
}

static bool already_analyzed(Store& s, const Hash& h) {
  Stmt st(s.db(), "SELECT 1 FROM analyzed WHERE hash = ?");
  st.bind(1, h);
  return st.step();
}

static void record_analysis(Store& s, const Hash& h, const Analysis& a, const Def* d) {
  if (d) {
    Stmt info(s.db(),
              "INSERT OR REPLACE INTO def_info(hash, lang, kind, name, lines, signature, doc) "
              "VALUES (?, ?, ?, ?, ?, ?, ?)");
    info.bind(1, h).bind(2, d->lang).bind(3, d->kind).bind(4, d->name)
        .bind(5, (long long)d->lines).bind(6, a.signature).bind(7, a.doc).run();
  }
  Stmt ref(s.db(), "INSERT INTO def_ref(hash, name, kind, line) VALUES (?, ?, 'call', ?)");
  for (auto& c : a.calls) {
    ref.bind(1, h).bind(2, c.name).bind(3, (long long)c.line).run();
    ref.reset();
  }
  Stmt imp(s.db(), "INSERT INTO def_import(hash, module, name, alias, level) VALUES (?, ?, ?, ?, ?)");
  for (auto& i : a.imports) {
    imp.bind(1, h).bind(2, i.module).bind(3, i.name).bind(4, i.alias).bind(5, (long long)i.level).run();
    imp.reset();
  }
  Stmt done(s.db(), "INSERT OR IGNORE INTO analyzed(hash) VALUES (?)");
  done.bind(1, h).run();
}

std::vector<Part> build_parts(Store& s, const Language& lang, std::string_view src,
                              const std::vector<Chunk>& chunks, size_t begin, size_t end) {
  std::vector<Part> parts;
  size_t pos = begin;
  auto text = [&](size_t a, size_t b) {
    if (b <= a) return;
    if (!parts.empty() && parts.back().type == Part::Text) parts.back().text += src.substr(a, b - a);
    else parts.push_back(Part{Part::Text, std::string(src.substr(a, b - a)), {}});
  };
  for (auto& c : chunks) {
    text(pos, c.begin);
    parts.push_back(Part{Part::Def, {}, build_def(s, lang, src, c)});
    pos = c.end;
  }
  text(pos, end);
  return parts;
}

Hash build_def(Store& s, const Language& lang, std::string_view src, const Chunk& c) {
  Def d;
  d.lang = lang.name();
  d.kind = c.kind;
  d.name = c.name;
  d.parts = build_parts(s, lang, src, c.children, c.begin, c.end);
  return finish_def(s, lang, std::move(d));
}

Hash finish_def(Store& s, const Language& lang, Def d) {
  std::string own = own_text(d);
  Analysis a = lang.analyze(own);
  d.norm = a.norm;
  d.lines = 0;  // non-blank lines: what a reviewer actually reads
  for (auto& l : split_lines(own))
    if (!trim(l).empty()) ++d.lines;
  Hash h = put_def(s, d);
  if (!already_analyzed(s, h)) record_analysis(s, h, a, &d);
  return h;
}

void analyze_file(Store& s, const Hash& file_hash, const FileTrail& f) {
  if (already_analyzed(s, file_hash)) return;
  const Language* lang = language_by_name(f.lang);
  if (!lang) return;
  std::string glue;
  for (auto& p : f.parts)
    if (p.type == Part::Text) glue += p.text;
  record_analysis(s, file_hash, lang->analyze(glue), nullptr);
}

FileEntry store_file(Store& s, const std::string& path, const std::string& bytes) {
  const Language* lang = language_for_path(path);
  if (!lang || !is_utf8(bytes)) return FileEntry{"blob", s.put("blob", bytes)};
  FileTrail f;
  f.lang = lang->name();
  f.parts = build_parts(s, *lang, bytes, lang->split(bytes), 0, bytes.size());
  Hash h = put_file(s, f);
  analyze_file(s, h, f);
  return FileEntry{"file", h};
}

}  // namespace hs
