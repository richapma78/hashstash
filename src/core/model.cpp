#include "core/model.h"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace hs {

using json = nlohmann::json;

static json parts_to_json(const std::vector<Part>& parts) {
  json a = json::array();
  for (auto& p : parts) {
    if (p.type == Part::Text) a.push_back(json::array({"t", p.text}));
    else if (p.type == Part::Def) a.push_back(json::array({"d", p.hash}));
    else a.push_back(json::array({"f", p.hash}));
  }
  return a;
}

static std::vector<Part> parts_from_json(const json& a) {
  std::vector<Part> out;
  for (auto& e : a) {
    Part p;
    std::string t = e.at(0);
    if (t == "t") {
      p.type = Part::Text;
      p.text = e.at(1);
    } else if (t == "d") {
      p.type = Part::Def;
      p.hash = e.at(1);
    } else {
      p.type = Part::Frag;
      p.hash = e.at(1);
    }
    out.push_back(std::move(p));
  }
  return out;
}

static json parse_kind(const Store& s, const Hash& h, const char* kind) {
  auto o = s.must_get(h);
  if (o.kind != kind)
    throw std::runtime_error("object #" + short_hash(h) + " is a " + o.kind + ", not a " + kind);
  return json::parse(o.body);
}

Hash put_def(Store& s, const Def& d) {
  json j = {{"lang", d.lang}, {"kind", d.kind}, {"name", d.name},
            {"norm", d.norm}, {"lines", d.lines}, {"parts", parts_to_json(d.parts)}};
  return s.put("def", j.dump());
}

Hash put_frag(Store& s, const Frag& f) {
  json j = {{"id", f.id}, {"label", f.label}, {"text", f.text}};
  return s.put("frag", j.dump());
}

Hash put_file(Store& s, const FileTrail& f) {
  json j = {{"lang", f.lang}, {"parts", parts_to_json(f.parts)}};
  return s.put("file", j.dump());
}

Hash put_namespace(Store& s, const Namespace& n) {
  json files = json::object();
  for (auto& [path, e] : n.files) files[path] = json::array({e.kind, e.hash});
  json j = {{"files", files}, {"lineage", n.lineage}};
  json tests = json::object();
  for (auto& [def, ts] : n.tests)
    if (!ts.empty()) tests[def] = ts;
  if (!tests.empty()) j["tests"] = tests;
  if (!n.attach.empty()) j["attach"] = n.attach;
  return s.put("namespace", j.dump());
}

Hash put_commit(Store& s, const Commit& c) {
  json j = {{"ns", c.ns},         {"parents", c.parents}, {"author", c.author},
            {"time", c.time},     {"message", c.message}, {"changes", c.changes}};
  return s.put("commit", j.dump());
}

Def load_def(const Store& s, const Hash& h) {
  json j = parse_kind(s, h, "def");
  Def d;
  d.lang = j.at("lang");
  d.kind = j.at("kind");
  d.name = j.at("name");
  d.norm = j.at("norm");
  d.lines = j.at("lines");
  d.parts = parts_from_json(j.at("parts"));
  return d;
}

Frag load_frag(const Store& s, const Hash& h) {
  json j = parse_kind(s, h, "frag");
  return Frag{j.at("id"), j.at("label"), j.at("text")};
}

FileTrail load_file(const Store& s, const Hash& h) {
  json j = parse_kind(s, h, "file");
  return FileTrail{j.at("lang"), parts_from_json(j.at("parts"))};
}

Namespace load_namespace(const Store& s, const Hash& h) {
  json j = parse_kind(s, h, "namespace");
  Namespace n;
  for (auto& [path, e] : j.at("files").items())
    n.files[path] = FileEntry{e.at(0), e.at(1)};
  n.lineage = j.at("lineage").get<std::map<std::string, std::string>>();
  if (j.contains("tests"))
    n.tests = j["tests"].get<std::map<std::string, std::set<std::string>>>();
  if (j.contains("attach")) n.attach = j["attach"].get<std::map<std::string, Hash>>();
  return n;
}

Hash put_attach(Store& s, const Attach& a) {
  json notes = json::array(), ctx = json::array();
  for (auto& n : a.notes)
    notes.push_back({{"id", n.id}, {"from", n.from}, {"to", n.to}, {"text", n.text},
                     {"author", n.author}, {"time", n.time}, {"stale", n.stale}});
  for (auto& c : a.context)
    ctx.push_back({{"id", c.id}, {"kind", c.kind}, {"text", c.text}, {"source", c.source},
                   {"author", c.author}, {"time", c.time}, {"def", c.def_hash}});
  return s.put("attach", json{{"notes", notes}, {"context", ctx}}.dump());
}

Attach load_attach(const Store& s, const Hash& h) {
  json j = parse_kind(s, h, "attach");
  Attach a;
  for (auto& n : j.at("notes"))
    a.notes.push_back(Note{n.at("id"), n.at("from"), n.at("to"), n.at("text"), n.at("author"),
                           n.at("time"), n.at("stale")});
  for (auto& c : j.at("context"))
    a.context.push_back(ContextEntry{c.at("id"), c.at("kind"), c.at("text"), c.at("source"),
                                     c.at("author"), c.at("time"), c.at("def")});
  return a;
}

Commit load_commit(const Store& s, const Hash& h) {
  json j = parse_kind(s, h, "commit");
  Commit c;
  c.ns = j.at("ns");
  c.parents = j.at("parents").get<std::vector<Hash>>();
  c.author = j.at("author");
  c.time = j.at("time");
  c.message = j.at("message");
  if (j.contains("changes")) c.changes = j["changes"].get<std::vector<std::string>>();
  return c;
}

std::string render_parts(const Store& s, const std::vector<Part>& parts) {
  std::string out;
  for (auto& p : parts) {
    if (p.type == Part::Text) out += p.text;
    else if (p.type == Part::Def) out += render_def(s, p.hash);
    else out += load_frag(s, p.hash).text;
  }
  return out;
}

std::string render_def(const Store& s, const Hash& h) { return render_parts(s, load_def(s, h).parts); }

std::string render_entry(const Store& s, const FileEntry& e) {
  if (e.kind == "blob") return s.must_get(e.hash).body;
  return render_parts(s, load_file(s, e.hash).parts);
}

std::string qualify(const std::string& file, const std::string& path) { return file + ":" + path; }

std::string member_name(const std::string& name, int occurrence) {
  return occurrence <= 1 ? name : name + "~" + std::to_string(occurrence);
}

int count_lines(const std::string& text) {
  if (text.empty()) return 0;
  int n = 0;
  for (char c : text) n += (c == '\n');
  if (text.back() != '\n') ++n;
  return n;
}

static void walk(const Store& s, const Namespace& n, const std::string& file,
                 const std::vector<Part>& parts, const std::string& prefix,
                 const std::string& parent, int& seq, std::vector<FlatDef>& out) {
  std::map<std::string, int> seen;
  for (auto& p : parts) {
    if (p.type != Part::Def) continue;
    FlatDef f;
    f.def = load_def(s, p.hash);
    f.hash = p.hash;
    f.file = file;
    std::string name = member_name(f.def.name, ++seen[f.def.name]);
    f.path = prefix.empty() ? name : prefix + "." + name;
    f.qname = qualify(file, f.path);
    f.parent = parent;
    f.seq = seq++;
    auto it = n.lineage.find(f.qname);
    if (it != n.lineage.end()) f.lineage = it->second;
    std::vector<Part> children = f.def.parts;
    std::string path = f.path, qname = f.qname;
    out.push_back(std::move(f));
    walk(s, n, file, children, path, qname, seq, out);
  }
}

std::vector<FlatDef> flatten_file(const Store& s, const Namespace& n, const std::string& file) {
  std::vector<FlatDef> out;
  auto it = n.files.find(file);
  if (it == n.files.end() || it->second.kind != "file") return out;
  int seq = 0;
  walk(s, n, file, load_file(s, it->second.hash).parts, "", "", seq, out);
  return out;
}

std::vector<FlatDef> flatten(const Store& s, const Namespace& n) {
  std::vector<FlatDef> out;
  for (auto& [path, e] : n.files) {
    if (e.kind != "file") continue;
    auto part = flatten_file(s, n, path);
    out.insert(out.end(), std::make_move_iterator(part.begin()),
               std::make_move_iterator(part.end()));
  }
  return out;
}

}  // namespace hs
