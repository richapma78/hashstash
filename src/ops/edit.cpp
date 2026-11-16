#include "ops/edit.h"

#include <functional>
#include <map>
#include <set>
#include <stdexcept>

#include "ops/build.h"
#include "ops/index.h"
#include "ops/locks.h"
#include "ops/context.h"
#include "ops/query.h"

namespace hs {

namespace {

struct QName {
  std::string file;
  std::vector<std::string> comps;  // "Server", "start"
};

QName parse_qname(const std::string& q) {
  size_t colon = q.find(':');
  if (colon == std::string::npos) throw std::runtime_error("not a qualified name: " + q);
  QName out{q.substr(0, colon), {}};
  std::string path = q.substr(colon + 1);
  size_t i = 0;
  while (i <= path.size()) {
    size_t d = path.find('.', i);
    if (d == std::string::npos) d = path.size();
    if (d > i) out.comps.push_back(path.substr(i, d - i));
    i = d + 1;
  }
  return out;
}

std::string join_path(const std::vector<std::string>& comps, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) out += (i ? "." : "") + comps[i];
  return out;
}

// Index of the Def part named comp ("x" or "x~2") in parts, or -1.
int find_member(const Store& s, const std::vector<Part>& parts, const std::string& comp) {
  std::map<std::string, int> seen;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (parts[i].type != Part::Def) continue;
    Def d = load_def(s, parts[i].hash);
    if (member_name(d.name, ++seen[d.name]) == comp) return (int)i;
  }
  return -1;
}

// Merges neighbouring Text parts and drops empty ones.
void tidy(std::vector<Part>& parts) {
  std::vector<Part> out;
  for (auto& p : parts) {
    if (p.type == Part::Text) {
      if (p.text.empty()) continue;
      if (!out.empty() && out.back().type == Part::Text) {
        out.back().text += p.text;
        continue;
      }
    }
    out.push_back(p);
  }
  parts = std::move(out);
}

struct Container {
  std::string kind;  // "file" or the definition kind
  std::string indent;  // member indentation
};

using Mutator = std::function<void(std::vector<Part>&, const Container&)>;

std::string leading_ws(const std::string& text) {
  std::string prefix;
  dedent(text, &prefix);
  return prefix;
}

// Applies fn to the parts of the container at comps[0..depth) and rebuilds every
// definition on the way back up. Returns the file's new top-level parts.
std::vector<Part> rewrite(Store& s, const Language& lang, std::vector<Part> parts,
                          const std::vector<std::string>& comps, size_t depth, size_t idx,
                          const Container& here, const Mutator& fn) {
  if (idx == depth) {
    fn(parts, here);
    tidy(parts);
    return parts;
  }
  int i = find_member(s, parts, comps[idx]);
  if (i < 0) throw std::runtime_error("no member '" + comps[idx] + "'");
  Def d = load_def(s, parts[i].hash);
  Container inner{d.kind, ""};
  // Member indentation: that of an existing child, else this one's plus 4 spaces.
  for (auto& p : d.parts)
    if (p.type == Part::Def) {
      inner.indent = leading_ws(render_def(s, p.hash));
      break;
    }
  if (inner.indent.empty()) inner.indent = leading_ws(render_parts(s, d.parts)) + "    ";
  d.parts = rewrite(s, lang, d.parts, comps, depth, idx + 1, inner, fn);
  parts[i].hash = finish_def(s, lang, std::move(d));
  return parts;
}

std::string to_lf(const std::string& t) {
  std::string out;
  out.reserve(t.size());
  for (size_t i = 0; i < t.size(); ++i)
    if (!(t[i] == '\r' && i + 1 < t.size() && t[i + 1] == '\n')) out += t[i];
  return out;
}

std::string from_lf(const std::string& t, const std::string& eol) {
  if (eol == "\n") return t;
  std::string out;
  for (char c : t) {
    if (c == '\n') out += eol;
    else out += c;
  }
  return out;
}

// Dedents, trims blank lines at both ends, and ends with one newline.
std::string clean_text(const std::string& text) {
  std::string t = dedent(to_lf(text));
  size_t a = 0;
  while (a < t.size()) {  // drop leading blank lines
    size_t eol = t.find('\n', a);
    if (eol == std::string::npos) break;
    if (t.substr(a, eol - a).find_first_not_of(" \t") != std::string::npos) break;
    a = eol + 1;
  }
  t = t.substr(a);
  size_t b = t.find_last_not_of(" \t\n");
  if (b == std::string::npos) return "";
  t.resize(b + 1);
  return t + "\n";
}

struct Built {
  std::vector<Part> parts;
  std::vector<Hash> defs;  // top-level definitions, in order
};

// Splits clean (dedented, LF) text into definitions, then stores them indented
// by prefix with the file's line endings.
Built build_text(Store& s, const Language& lang, const std::string& clean, const std::string& prefix,
                 const std::string& eol, bool in_class, bool final_newline) {
  auto chunks = lang.split(clean);
  std::string final = from_lf(indent(clean, prefix), eol);
  if (!final_newline && ends_with(final, eol)) final.resize(final.size() - eol.size());
  auto src_starts = line_starts(clean);
  auto dst_starts = line_starts(final);
  auto line_of = [&](size_t off) {
    return (size_t)(std::upper_bound(src_starts.begin(), src_starts.end(), off) - src_starts.begin() - 1);
  };
  auto map_off = [&](size_t off) {
    if (off >= clean.size()) return final.size();
    size_t l = line_of(off);
    return l < dst_starts.size() ? dst_starts[l] : final.size();
  };
  std::function<void(Chunk&, bool)> remap = [&](Chunk& c, bool cls) {
    c.begin = map_off(c.begin);
    c.end = map_off(c.end);
    if (cls && c.kind == "function") c.kind = "method";
    for (auto& k : c.children) remap(k, c.kind == "class");
  };
  for (auto& c : chunks) remap(c, in_class);
  Built b;
  b.parts = build_parts(s, lang, final, chunks, 0, final.size());
  for (auto& p : b.parts)
    if (p.type == Part::Def) b.defs.push_back(p.hash);
  return b;
}

std::string eol_of(const std::string& text) {
  return text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
}

bool max_lines_ok(Store& s, const Repo& repo, const std::vector<Hash>& defs, std::string& err) {
  int max = repo.config().max_lines;
  if (max <= 0) return true;
  std::function<bool(const Hash&)> ok = [&](const Hash& h) {
    Def d = load_def(s, h);
    if (d.lines > max) {
      err = d.name + " has " + std::to_string(d.lines) + " lines; the project limit is " +
            std::to_string(max) + ". Split it into smaller helper functions.";
      return false;
    }
    for (auto& p : d.parts)
      if (p.type == Part::Def && !ok(p.hash)) return false;
    return true;
  };
  for (auto& h : defs)
    if (!ok(h)) return false;
  return true;
}

// Qualified names of the given top-level hashes (and their children) inside container.
std::vector<FlatDef> defs_with_hashes(const Store& s, const Namespace& ns, const std::string& file,
                                      const std::string& container_q, const std::vector<Hash>& hashes) {
  std::set<Hash> want(hashes.begin(), hashes.end());
  std::vector<FlatDef> out;
  std::set<std::string> roots;
  for (auto& f : flatten_file(s, ns, file)) {
    bool root = f.parent == container_q && want.count(f.hash);
    bool child = roots.count(f.parent) > 0;
    if (root || child) {
      roots.insert(f.qname);
      out.push_back(f);
    }
  }
  return out;
}

const Language& lang_of_file(const Namespace& ns, const Store& s, const std::string& file) {
  auto it = ns.files.find(file);
  if (it == ns.files.end()) throw std::runtime_error("no file " + file);
  if (it->second.kind != "file") throw std::runtime_error(file + " is not split into definitions");
  const Language* l = language_by_name(load_file(s, it->second.hash).lang);
  if (!l) throw std::runtime_error("no language adapter for " + file);
  return *l;
}

std::string lineage_of(const Namespace& ns, const std::string& q) {
  auto it = ns.lineage.find(q);
  return it == ns.lineage.end() ? "" : it->second;
}

void check_unlocked(Repo& repo, const Namespace& ns, const std::string& q) {
  std::string id = lineage_of(ns, q);
  if (id.empty()) return;
  if (auto b = blocking_lock(repo, id))
    throw std::runtime_error(q + " is locked by " + b->holder + " until " + format_time(b->expires));
}

// Moves lineage ids from old to new qualified names, including children.
void move_lineage(Namespace& ns, const std::string& from, const std::string& to) {
  if (from == to) return;
  std::map<std::string, std::string> moved;
  for (auto it = ns.lineage.begin(); it != ns.lineage.end();) {
    if (it->first == from || starts_with(it->first, from + ".")) {
      moved[to + it->first.substr(from.size())] = it->second;
      it = ns.lineage.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& [k, v] : moved) ns.lineage[k] = v;
}

void note_context(Repo& repo, Namespace& ns, const std::string& lineage, const std::string& kind,
                  const std::string& text, const Hash& def_hash) {
  Attach a = attach_of(repo.store(), ns, lineage);
  a.context.push_back(ContextEntry{new_uuid().substr(0, 8), kind, trim(text), "", repo.agent(),
                                   now_seconds(), def_hash});
  ns.attach[lineage] = put_attach(repo.store(), a);
}

EditOutcome guarded(const std::function<void(EditOutcome&)>& fn) {
  EditOutcome o;
  try {
    fn(o);
    o.ok = true;
  } catch (std::exception& e) {
    o.ok = false;
    o.error = e.what();
  }
  return o;
}

}  // namespace

EditOutcome replace_def(Repo& repo, Namespace& ns_io, const std::string& qname,
                        const std::string& text, const std::string& base, const std::string& reason) {
  return guarded([&](EditOutcome& o) {
    Store& s = repo.store();
    Namespace ns = ns_io;
    QName q = parse_qname(qname);
    const Language& lang = lang_of_file(ns, s, q.file);
    check_unlocked(repo, ns, qname);

    FileTrail trail = load_file(s, ns.files[q.file].hash);
    // Find the current definition and its container.
    std::vector<Part>* level = &trail.parts;
    std::vector<Def> chain;
    Hash current;
    for (size_t i = 0; i < q.comps.size(); ++i) {
      int k = find_member(s, *level, q.comps[i]);
      if (k < 0) throw std::runtime_error("no definition " + qname);
      if (i + 1 == q.comps.size()) current = (*level)[k].hash;
      else {
        chain.push_back(load_def(s, (*level)[k].hash));
        level = &chain.back().parts;
      }
    }
    if (!base.empty() && !starts_with(current, base))
      throw std::runtime_error(qname + " changed since it was opened (now #" + short_hash(current) +
                               "); re-open it and reapply your change");
    Def old = load_def(s, current);
    std::string old_text = render_def(s, current);
    std::string clean = clean_text(text);
    if (clean.empty()) throw std::runtime_error("no code given for " + qname + " (use delete to remove it)");
    bool in_class = !chain.empty() && chain.back().kind == "class";
    Built b = build_text(s, lang, clean, leading_ws(old_text), eol_of(old_text), in_class,
                         ends_with(old_text, "\n"));
    if (b.defs.empty()) throw std::runtime_error("no definition found in the new code for " + qname);
    std::string err;
    if (!max_lines_ok(s, repo, b.defs, err)) throw std::runtime_error(err);

    size_t depth = q.comps.size() - 1;
    std::string target = q.comps.back();
    trail.parts = rewrite(s, lang, load_file(s, ns.files[q.file].hash).parts, q.comps, depth, 0,
                          Container{"file", ""}, [&](std::vector<Part>& parts, const Container&) {
                            int k = find_member(s, parts, target);
                            if (k < 0) throw std::runtime_error("no definition " + qname);
                            parts.erase(parts.begin() + k);
                            parts.insert(parts.begin() + k, b.parts.begin(), b.parts.end());
                          });
    FileEntry e{"file", put_file(s, trail)};
    analyze_file(s, e.hash, trail);
    ns.files[q.file] = e;

    // Which new definition carries the old lineage: same name, else the only one.
    std::string container_q = depth ? qualify(q.file, join_path(q.comps, depth)) : "";
    auto fresh = defs_with_hashes(s, ns, q.file, container_q, b.defs);
    const FlatDef* heir = nullptr;
    for (auto& f : fresh)
      if (f.parent == container_q && f.def.name == old.name) {
        heir = &f;
        break;
      }
    if (!heir && b.defs.size() == 1)
      for (auto& f : fresh)
        if (f.parent == container_q) heir = &f;
    std::string old_lineage = lineage_of(ns, qname);
    if (heir) move_lineage(ns, qname, heir->qname);
    fix_lineage(s, ns);
    if (heir && !old_lineage.empty()) {
      remap_notes(s, ns, old_lineage, old_text, render_def(s, heir->hash));
      if (!trim(reason).empty()) note_context(repo, ns, old_lineage, "change", reason, heir->hash);
    }

    for (auto& f : fresh) o.changed.push_back(f.qname);
    if (!heir || heir->qname != qname) o.removed.push_back(qname);
    o.size_warnings = size_warnings(repo, fresh);
    ns_io = std::move(ns);
  });
}

EditOutcome add_def(Repo& repo, Namespace& ns_io, const std::string& file,
                    const std::string& container, const std::string& after,
                    const std::string& text, const std::string& reason) {
  return guarded([&](EditOutcome& o) {
    Store& s = repo.store();
    Namespace ns = ns_io;
    std::string clean = clean_text(text);
    if (clean.empty()) throw std::runtime_error("no code given");
    std::vector<Hash> new_defs;
    std::string container_q = container.empty() ? "" : qualify(file, container);

    if (!ns.files.count(file)) {
      if (!container.empty()) throw std::runtime_error("no file " + file);
      const Language* lang = language_for_path(file);
      if (!lang) throw std::runtime_error("no language adapter for " + file);
      FileEntry e = store_file(s, file, clean);
      ns.files[file] = e;
      for (auto& p : load_file(s, e.hash).parts)
        if (p.type == Part::Def) new_defs.push_back(p.hash);
    } else {
      const Language& lang = lang_of_file(ns, s, file);
      if (!container_q.empty()) check_unlocked(repo, ns, container_q);
      QName cq = container.empty() ? QName{file, {}} : parse_qname(container_q);
      FileTrail trail = load_file(s, ns.files[file].hash);
      std::string eol = eol_of(render_parts(s, trail.parts));
      trail.parts = rewrite(
          s, lang, trail.parts, cq.comps, cq.comps.size(), 0, Container{"file", ""},
          [&](std::vector<Part>& parts, const Container& c) {
            bool in_class = c.kind == "class";
            Built b = build_text(s, lang, clean, c.indent, eol, in_class, true);
            if (b.defs.empty()) throw std::runtime_error("no definition found in the new code");
            std::string err;
            if (!max_lines_ok(s, repo, b.defs, err)) throw std::runtime_error(err);
            new_defs = b.defs;
            // Insert after the named member, else after the last definition.
            int at = -1;
            if (!after.empty()) {
              at = find_member(s, parts, after);
              if (at < 0) throw std::runtime_error("no member '" + after + "' to insert after");
            } else {
              for (int i = (int)parts.size() - 1; i >= 0; --i)
                if (parts[i].type == Part::Def) {
                  at = i;
                  break;
                }
            }
            std::string sep = from_lf(c.kind == "file" ? "\n\n" : "\n", eol);
            std::vector<Part> ins;
            if (at < 0) {  // no definitions yet: append at the end
              std::string tail = render_parts(s, parts);
              if (!tail.empty() && !ends_with(tail, "\n")) ins.push_back({Part::Text, eol, {}});
              if (!tail.empty()) ins.push_back({Part::Text, sep, {}});
              ins.insert(ins.end(), b.parts.begin(), b.parts.end());
              parts.insert(parts.end(), ins.begin(), ins.end());
              return;
            }
            std::string prev = render_def(s, parts[at].hash);
            if (!ends_with(prev, "\n")) ins.push_back({Part::Text, eol, {}});
            ins.push_back({Part::Text, sep, {}});
            ins.insert(ins.end(), b.parts.begin(), b.parts.end());
            parts.insert(parts.begin() + at + 1, ins.begin(), ins.end());
          });
      FileEntry e{"file", put_file(s, trail)};
      analyze_file(s, e.hash, trail);
      ns.files[file] = e;
    }
    fix_lineage(s, ns);
    auto fresh = defs_with_hashes(s, ns, file, container_q, new_defs);
    if (!trim(reason).empty())
      for (auto& f : fresh)
        if (f.parent == container_q) note_context(repo, ns, ns.lineage[f.qname], "origin", reason, f.hash);
    for (auto& f : fresh) o.changed.push_back(f.qname);
    o.size_warnings = size_warnings(repo, fresh);
    ns_io = std::move(ns);
  });
}

EditOutcome delete_def(Repo& repo, Namespace& ns_io, const std::string& qname) {
  return guarded([&](EditOutcome& o) {
    Store& s = repo.store();
    Namespace ns = ns_io;
    QName q = parse_qname(qname);
    const Language& lang = lang_of_file(ns, s, q.file);
    check_unlocked(repo, ns, qname);
    FileTrail trail = load_file(s, ns.files[q.file].hash);
    size_t depth = q.comps.size() - 1;
    trail.parts = rewrite(s, lang, trail.parts, q.comps, depth, 0, Container{"file", ""},
                          [&](std::vector<Part>& parts, const Container&) {
                            int k = find_member(s, parts, q.comps.back());
                            if (k < 0) throw std::runtime_error("no definition " + qname);
                            parts.erase(parts.begin() + k);
                            // Avoid doubling the blank lines on either side.
                            if (k > 0 && k < (int)parts.size() && parts[k - 1].type == Part::Text &&
                                parts[k].type == Part::Text) {
                              std::string& prev = parts[k - 1].text;
                              std::string& next = parts[k].text;
                              bool prev_blank = ends_with(to_lf(prev), "\n\n") || to_lf(prev) == "\n";
                              if (prev_blank) {
                                size_t a = 0;
                                while (a < next.size()) {
                                  size_t eol = next.find('\n', a);
                                  if (eol == std::string::npos) break;
                                  if (next.substr(a, eol - a).find_first_not_of(" \t\r") != std::string::npos) break;
                                  a = eol + 1;
                                }
                                next = next.substr(a);
                              }
                            }
                          });
    FileEntry e{"file", put_file(s, trail)};
    analyze_file(s, e.hash, trail);
    ns.files[q.file] = e;
    fix_lineage(s, ns);
    o.removed.push_back(qname);
    ns_io = std::move(ns);
  });
}

void commit_edits(Repo& repo, const Namespace& ns, std::vector<EditOutcome*> outcomes,
                  const std::string& reason) {
  std::string summary = reason;
  for (auto* o : outcomes) {
    if (!o->ok) continue;
    for (auto& q : o->changed) summary += (summary.empty() ? "" : "; ") + std::string("stored ") + q;
    for (auto& q : o->removed) summary += (summary.empty() ? "" : "; ") + std::string("removed ") + q;
  }
  if (summary.size() > 500) summary = summary.substr(0, 497) + "...";
  repo.set_work_ns(ns, "update", summary);
  renew_locks(repo);
  for (auto* o : outcomes) {
    if (!o->ok) continue;
    o->test_warnings = missing_tests(repo, ns, &o->changed);
  }
}

// ------------------------------------------------------------------ scratch

static std::string ext_for(const std::string& lang) {
  if (lang == "python") return "py";
  if (lang == "gdscript") return "gd";
  if (lang == "godot") return "tscn";
  return lang;
}

static const char* kScratchHeader =
    "{c} hashstash scratch file. Edit the definitions below, then run: hs update\n"
    "{c} Each section starts with a \"{c} @hs:\" line; keep those lines as they are.\n"
    "{c} A section may hold several definitions (e.g. a function split into helpers).\n"
    "{c} Add code:     {c} @hs:new <file>[:<Class>] [after=<name>]\n"
    "{c} Delete code:  {c} @hs:delete <file>:<name>\n";

static std::string header_for(const Language& lang) {
  std::string h = kScratchHeader, c = lang.comment_prefix(), out;
  for (size_t i = 0; i < h.size(); ++i) {
    if (h.compare(i, 3, "{c}") == 0) {
      out += c;
      i += 2;
    } else {
      out += h[i];
    }
  }
  return out;
}

std::vector<fs::path> scratch_files(Repo& repo) {
  std::vector<fs::path> out;
  if (!fs::exists(repo.scratch_dir())) return out;
  for (auto& e : fs::directory_iterator(repo.scratch_dir()))
    if (e.is_regular_file() && starts_with(to_generic(e.path().filename()), "edit.")) out.push_back(e.path());
  return out;
}

static fs::path scratch_for(Repo& repo, const Language& lang) {
  return repo.scratch_dir() / ("edit." + ext_for(lang.name()));
}

static void append_section(Repo& repo, const Language& lang, const std::string& header,
                           const std::string& body) {
  fs::path p = scratch_for(repo, lang);
  std::string cur = fs::exists(p) ? read_file(p) : header_for(lang);
  if (cur.find(header) != std::string::npos) return;
  if (!ends_with(cur, "\n")) cur += "\n";
  cur += "\n" + header + "\n" + body;
  if (!ends_with(cur, "\n")) cur += "\n";
  write_file(p, cur);
}

std::vector<fs::path> open_for_edit(Repo& repo, const std::vector<std::string>& names) {
  std::set<fs::path> files;
  Store& s = repo.store();
  for (auto& n : names) {
    IndexRow r = resolve_name(repo, n);
    if (r.kind == "module") throw std::runtime_error("can't edit module code yet: " + r.qname);
    acquire_lock(repo, r.lineage, r.qname);
    const Language* lang = language_by_name(r.lang);
    if (!lang) throw std::runtime_error("no language adapter for " + r.qname);
    std::string body = clean_text(render_def(s, r.hash));
    std::string header = lang->comment_prefix() + " @hs:edit " + r.qname + " base=" + short_hash(r.hash);
    append_section(repo, *lang, header, body);
    files.insert(scratch_for(repo, *lang));
  }
  return {files.begin(), files.end()};
}

fs::path open_new(Repo& repo, const std::string& where, const std::string& after) {
  std::string file = where.substr(0, where.find(':'));
  const Language* lang = language_for_path(file);
  if (!lang) throw std::runtime_error("no language adapter for " + file);
  std::string header = lang->comment_prefix() + " @hs:new " + where + (after.empty() ? "" : " after=" + after);
  append_section(repo, *lang, header, "");
  return scratch_for(repo, *lang);
}

std::vector<ScratchResult> apply_scratch(Repo& repo, const std::string& reason) {
  std::vector<ScratchResult> results;
  Namespace ns = repo.work_ns();
  bool any_ok = false;
  std::vector<std::pair<fs::path, std::string>> rewrites;

  for (auto& path : scratch_files(repo)) {
    std::string content = read_file(path);
    auto lines = split_lines(content);
    // Split into preamble and sections at "<comment> @hs:" lines.
    struct Section {
      std::string header, body;
    };
    std::vector<Section> sections;
    std::string preamble;
    for (auto& l : lines) {
      std::string t = trim(l);
      size_t at = t.find("@hs:");
      bool header = at != std::string::npos && at <= 3 && t.find_first_not_of("#/;- ") == at &&
                    (starts_with(t.substr(at), "@hs:edit ") || starts_with(t.substr(at), "@hs:new ") ||
                     starts_with(t.substr(at), "@hs:delete "));
      if (header) sections.push_back({t, ""});
      else if (sections.empty()) preamble += l;
      else sections.back().body += l;
    }
    std::string keep = preamble;
    for (auto& sec : sections) {
      std::string cmd = sec.header.substr(sec.header.find("@hs:") + 4);
      std::vector<std::string> words;
      size_t i = 0;
      while (i < cmd.size()) {
        size_t sp = cmd.find(' ', i);
        if (sp == std::string::npos) sp = cmd.size();
        if (sp > i) words.push_back(cmd.substr(i, sp - i));
        i = sp + 1;
      }
      auto opt = [&](const std::string& key) {
        for (auto& w : words)
          if (starts_with(w, key + "=")) return w.substr(key.size() + 1);
        return std::string();
      };
      EditOutcome o;
      if (words.size() < 2) {
        o.error = "malformed section header";
      } else if (words[0] == "edit") {
        o = replace_def(repo, ns, words[1], sec.body, opt("base"), reason);
      } else if (words[0] == "delete") {
        o = delete_def(repo, ns, words[1]);
      } else {  // new
        std::string where = words[1];
        size_t colon = where.find(':');
        std::string file = where.substr(0, colon);
        std::string container = colon == std::string::npos ? "" : where.substr(colon + 1);
        if (trim(sec.body).empty()) {
          o.ok = false;
          o.error = "empty";  // untouched placeholder: keep it, quietly
        } else {
          o = add_def(repo, ns, file, container, opt("after"), sec.body, reason);
        }
      }
      if (o.ok) any_ok = true;
      else keep += "\n" + sec.header + "\n" + sec.body;
      if (o.error != "empty") results.push_back({sec.header, o});
    }
    bool remaining = keep.size() > preamble.size();
    rewrites.push_back({path, remaining ? keep : ""});
  }
  if (any_ok) {
    std::vector<EditOutcome*> outs;
    for (auto& r : results) outs.push_back(&r.outcome);
    commit_edits(repo, ns, outs, reason);
  }
  for (auto& [p, text] : rewrites) {
    if (text.empty()) fs::remove(p);
    else write_file(p, text);
  }
  return results;
}

}  // namespace hs
