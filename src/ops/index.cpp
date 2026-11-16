#include "ops/index.h"

#include <map>
#include <set>
#include <unordered_map>

#include "ops/build.h"
#include "ops/repo.h"

namespace hs {

std::string module_node(const std::string& file) { return file + ":<module>"; }

void fix_lineage(const Store& s, Namespace& ns) {
  std::set<std::string> live;
  for (auto& f : flatten(s, ns)) {
    live.insert(f.qname);
    if (!ns.lineage.count(f.qname)) ns.lineage[f.qname] = new_uuid();
  }
  for (auto it = ns.lineage.begin(); it != ns.lineage.end();)
    it = live.count(it->first) ? std::next(it) : ns.lineage.erase(it);
  // Test links only between definitions that still exist.
  std::set<std::string> ids;
  for (auto& [q, id] : ns.lineage) ids.insert(id);
  for (auto it = ns.tests.begin(); it != ns.tests.end();) {
    if (!ids.count(it->first)) {
      it = ns.tests.erase(it);
      continue;
    }
    for (auto t = it->second.begin(); t != it->second.end();)
      t = ids.count(*t) ? std::next(t) : it->second.erase(t);
    it = it->second.empty() ? ns.tests.erase(it) : std::next(it);
  }
  for (auto it = ns.attach.begin(); it != ns.attach.end();)
    it = ids.count(it->first) ? std::next(it) : ns.attach.erase(it);
}

namespace {

struct Node {
  std::string qname, file, path, parent, kind, hash, lang;
};

std::vector<std::string> split_dots(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (true) {
    size_t d = s.find('.', i);
    out.push_back(s.substr(i, d == std::string::npos ? std::string::npos : d - i));
    if (d == std::string::npos) break;
    i = d + 1;
  }
  return out;
}

std::string join(const std::vector<std::string>& v, size_t from, size_t to, char sep = '.') {
  std::string out;
  for (size_t i = from; i < to; ++i) {
    if (i > from) out += sep;
    out += v[i];
  }
  return out;
}

// Python module names for a file path: "src/pkg/mod.py" -> src.pkg.mod, pkg.mod, mod.
std::vector<std::string> module_names(const std::string& file) {
  std::string m = file;
  for (const char* ext : {".pyi", ".py"})
    if (ends_with(m, ext)) {
      m.resize(m.size() - std::string(ext).size());
      break;
    }
  if (ends_with(m, "/__init__")) m.resize(m.size() - 9);
  for (auto& c : m)
    if (c == '/') c = '.';
  std::vector<std::string> out;
  auto parts = split_dots(m);
  for (size_t i = 0; i < parts.size(); ++i) out.push_back(join(parts, i, parts.size()));
  return out;
}

// The package a file belongs to, as path components ("pkg/sub/mod.py" -> pkg, sub).
std::vector<std::string> package_of(const std::string& file) {
  std::vector<std::string> parts;
  size_t i = 0;
  while (true) {
    size_t s = file.find('/', i);
    if (s == std::string::npos) break;
    parts.push_back(file.substr(i, s - i));
    i = s + 1;
  }
  return parts;
}

struct Resolver {
  std::vector<Node> nodes;
  std::map<std::pair<std::string, std::string>, std::string> by_file_path;  // (file, path) -> qname
  std::unordered_map<std::string, std::vector<std::string>> top_by_short;
  std::unordered_map<std::string, std::vector<std::string>> member_by_short;
  std::unordered_map<std::string, std::vector<std::string>> files_by_module;
  std::unordered_map<std::string, const Node*> by_qname;

  struct Target {
    std::string file;  // module file
    std::string path;  // "" when the alias names the module itself
  };

  std::optional<std::string> file_for_module(const std::string& module, int level,
                                             const std::string& from_file) {
    std::string full = module;
    if (level > 0) {
      auto pkg = package_of(from_file);
      if ((int)pkg.size() < level - 1) return std::nullopt;
      pkg.resize(pkg.size() - (level - 1));
      std::string base = join(pkg, 0, pkg.size());
      full = base.empty() ? module : (module.empty() ? base : base + "." + module);
      // Exact path match for relative imports.
      std::string p = full;
      for (auto& c : p)
        if (c == '.') c = '/';
      auto it = files_by_module.find(full);
      if (it != files_by_module.end())
        for (auto& f : it->second)
          if (f == p + ".py" || f == p + "/__init__.py" || f == p + ".pyi") return f;
    }
    auto it = files_by_module.find(full);
    if (it == files_by_module.end() || it->second.empty()) return std::nullopt;
    if (it->second.size() == 1) return it->second[0];
    // Prefer the shortest path (closest to the project root).
    std::string best = it->second[0];
    for (auto& f : it->second)
      if (f.size() < best.size()) best = f;
    return best;
  }

  std::unordered_map<std::string, Target> aliases(const std::vector<Import>& imports,
                                                  const std::string& file) {
    std::unordered_map<std::string, Target> out;
    for (auto& im : imports) {
      if (im.name.empty()) {  // import a.b [as c]
        auto f = file_for_module(im.module, im.level, file);
        if (!f) continue;
        if (!im.alias.empty()) out[im.alias] = {*f, ""};
        else out[im.module] = {*f, ""};  // used as "a.b.func"
      } else if (im.name == "*") {
        auto f = file_for_module(im.module, im.level, file);
        if (!f) continue;
        for (auto& n : nodes)
          if (n.file == *f && n.parent.empty() && n.kind != "module") out[n.path] = {*f, n.path};
      } else {  // from m import x [as y]
        std::string local = im.alias.empty() ? im.name : im.alias;
        auto f = file_for_module(im.module, im.level, file);
        if (f && by_file_path.count({*f, im.name})) {
          out[local] = {*f, im.name};
          continue;
        }
        std::string sub = im.module.empty() ? im.name : im.module + "." + im.name;
        if (auto g = file_for_module(sub, im.level, file)) out[local] = {*g, ""};
      }
    }
    return out;
  }

  struct Hit {
    std::string callee;
    std::string confidence;
  };

  // C++: definitions by their last name segment ("demo.Shape::normalize" -> "normalize").
  std::unordered_map<std::string, std::vector<std::string>> cpp_by_short;

  static std::string cpp_short(const std::string& s) {
    std::string last = s.substr(s.rfind('.') + 1);
    size_t c = last.rfind("::");
    if (c != std::string::npos) last = last.substr(c + 2);
    return last.substr(0, last.find('~') == 0 ? std::string::npos : last.find('~'));
  }

  template <class Exact>
  std::vector<Hit> resolve_cpp(const Node& caller, const std::string& name, Exact exact) {
    // Enclosing scopes of the caller, innermost first: "demo.Shape::f" gives
    // "demo.Shape", "demo", "". Each is tried as a prefix of the called name.
    std::vector<std::string> scopes;
    std::string p = caller.path;
    while (true) {
      size_t dot = p.rfind('.'), colons = p.rfind("::");
      size_t cut = std::string::npos;
      if (dot != std::string::npos && (colons == std::string::npos || dot > colons)) cut = dot;
      else if (colons != std::string::npos) cut = colons;
      if (cut == std::string::npos) break;
      p = p.substr(0, cut);
      scopes.push_back(p);
    }
    // "Shape::f" also scopes to its class written inside the same namespace.
    std::string own = caller.path.substr(caller.path.rfind('.') + 1);
    if (own.find("::") != std::string::npos) {
      std::string cls = own.substr(0, own.rfind("::"));
      std::string ns = caller.path.size() > own.size() ? caller.path.substr(0, caller.path.size() - own.size() - 1) : "";
      scopes.insert(scopes.begin(), ns.empty() ? cls : ns + "." + cls);
    }
    scopes.push_back("");
    std::string dotted = name;
    for (size_t c; (c = dotted.find("::")) != std::string::npos;) dotted.replace(c, 2, ".");
    for (auto& sc : scopes) {
      for (const std::string& n : {name, dotted}) {
        std::string path = sc.empty() ? n : sc + "." + n;
        if (auto h = exact(caller.file, path)) return {*h};
      }
      // A member declared in class "demo.Shape" but defined as "demo.Shape::name".
      if (!sc.empty())
        if (auto h = exact(caller.file, sc + "::" + name)) return {*h};
    }
    return by_short(cpp_by_short, cpp_short(dotted));
  }

  std::vector<Hit> by_short(const std::unordered_map<std::string, std::vector<std::string>>& index,
                            const std::string& name) {
    auto it = index.find(name);
    if (it == index.end() || it->second.empty()) return {{"", "external"}};
    if (it->second.size() == 1) return {{it->second[0], "likely"}};
    std::vector<Hit> out;
    for (size_t i = 0; i < it->second.size() && i < 5; ++i) out.push_back({it->second[i], "ambiguous"});
    return out;
  }

  std::vector<Hit> resolve(const Node& caller, const std::string& name,
                           const std::unordered_map<std::string, Target>& alias) {
    auto parts = split_dots(name);
    auto exact = [&](const std::string& file, const std::string& path) -> std::optional<Hit> {
      auto it = by_file_path.find({file, path});
      if (it == by_file_path.end()) return std::nullopt;
      return Hit{it->second, "exact"};
    };
    if (caller.lang == "cpp") return resolve_cpp(caller, name, exact);
    if (parts.size() == 1) {
      if (auto h = exact(caller.file, name)) return {*h};
      auto a = alias.find(name);
      if (a != alias.end() && !a->second.path.empty())
        if (auto h = exact(a->second.file, a->second.path)) return {*h};
      return by_short(top_by_short, name);
    }
    if ((parts[0] == "self" || parts[0] == "cls") && parts.size() == 2) {
      // Nearest enclosing class of the caller.
      const Node* n = &caller;
      while (n && n->kind != "class") {
        auto it = by_qname.find(n->parent);
        n = it == by_qname.end() ? nullptr : it->second;
      }
      if (n)
        if (auto h = exact(n->file, n->path + "." + parts[1])) return {*h};
      // A GDScript file is itself a class: self.f() in it calls its top-level f.
      if (!n)
        if (auto h = exact(caller.file, parts[1])) return {*h};
      return by_short(member_by_short, parts[1]);
    }
    // Longest prefix naming an imported module or definition.
    for (size_t i = parts.size() - 1; i >= 1; --i) {
      auto a = alias.find(join(parts, 0, i));
      if (a == alias.end()) continue;
      std::string rest = join(parts, i, parts.size());
      std::string path = a->second.path.empty() ? rest : a->second.path + "." + rest;
      if (auto h = exact(a->second.file, path)) return {*h};
      break;
    }
    // A class defined in the same file: Class.method(...)
    if (auto h = exact(caller.file, name)) return {*h};
    return by_short(member_by_short, parts.back());
  }
};

}  // namespace

void rebuild_index(Repo& repo) {
  Store& s = repo.store();
  Namespace ns = repo.work_ns();
  auto flat = flatten(s, ns);

  s.transaction([&] {
    s.exec("DELETE FROM name_index; DELETE FROM name_fts; DELETE FROM edge;");
    Stmt ins(s.db(),
             "INSERT OR REPLACE INTO name_index(qname, short, file, hash, lineage, kind, lang, parent, seq) "
             "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    Stmt fts(s.db(), "INSERT INTO name_fts(qname, short, signature, doc) VALUES (?, ?, ?, ?)");
    Stmt info(s.db(), "SELECT signature, doc FROM def_info WHERE hash = ?");

    Resolver r;
    r.nodes.reserve(flat.size() + ns.files.size());
    for (auto& f : flat) {
      ensure_analyzed(s, f.hash, f.def);
      ins.bind(1, f.qname).bind(2, f.def.name).bind(3, f.file).bind(4, f.hash).bind(5, f.lineage)
          .bind(6, f.def.kind).bind(7, f.def.lang).bind(8, f.parent).bind(9, (long long)f.seq).run();
      ins.reset();
      std::string sig, doc;
      info.bind(1, f.hash);
      if (info.step()) {
        sig = info.text(0);
        doc = info.text(1);
      }
      info.reset();
      fts.bind(1, f.qname).bind(2, f.def.name).bind(3, sig).bind(4, doc).run();
      fts.reset();
      r.nodes.push_back({f.qname, f.file, f.path, f.parent, f.def.kind, f.hash, f.def.lang});
    }
    for (auto& [path, e] : ns.files) {
      if (e.kind != "file") continue;
      FileTrail ft = load_file(s, e.hash);
      analyze_file(s, e.hash, ft);
      std::string q = module_node(path);
      ins.bind(1, q).bind(2, "<module>").bind(3, path).bind(4, e.hash).bind(5, "")
          .bind(6, "module").bind(7, ft.lang).bind(8, "").bind(9, -1LL).run();
      ins.reset();
      r.nodes.push_back({q, path, "<module>", "", "module", e.hash, ft.lang});
      for (auto& m : module_names(path)) r.files_by_module[m].push_back(path);
    }
    for (auto& n : r.nodes) {
      r.by_qname[n.qname] = &n;
      if (n.kind == "module") continue;
      r.by_file_path[{n.file, n.path}] = n.qname;
      std::string shortname = n.path.substr(n.path.rfind('.') + 1);
      if (n.lang == "cpp") {
        if (n.kind != "namespace") r.cpp_by_short[r.cpp_short(n.path)].push_back(n.qname);
        continue;
      }
      if (n.parent.empty()) r.top_by_short[n.path].push_back(n.qname);
      else r.member_by_short[shortname].push_back(n.qname);
    }

    // Imports per file (module-level) and per definition (local imports).
    Stmt imp(s.db(), "SELECT module, name, alias, level FROM def_import WHERE hash = ?");
    auto imports_of = [&](const std::string& h) {
      std::vector<Import> out;
      imp.bind(1, h);
      while (imp.step()) out.push_back({imp.text(0), imp.text(1), imp.text(2), (int)imp.integer(3)});
      imp.reset();
      return out;
    };
    std::unordered_map<std::string, std::vector<Import>> file_imports;
    for (auto& [path, e] : ns.files)
      if (e.kind == "file") file_imports[path] = imports_of(e.hash);

    Stmt refs(s.db(), "SELECT name, line FROM def_ref WHERE hash = ?");
    Stmt edge(s.db(), "INSERT INTO edge(caller, callee, name, line, confidence) VALUES (?, ?, ?, ?, ?)");
    std::unordered_map<std::string, std::unordered_map<std::string, Resolver::Target>> file_alias;
    for (auto& n : r.nodes) {
      auto fa = file_alias.find(n.file);
      if (fa == file_alias.end())
        fa = file_alias.emplace(n.file, r.aliases(file_imports[n.file], n.file)).first;
      auto local = imports_of(n.hash);
      std::unordered_map<std::string, Resolver::Target> alias = fa->second;
      if (!local.empty() && n.kind != "module")
        for (auto& [k, v] : r.aliases(local, n.file)) alias[k] = v;
      refs.bind(1, n.hash);
      while (refs.step()) {
        std::string name = refs.text(0);
        long long line = refs.integer(1);
        for (auto& hit : r.resolve(n, name, alias)) {
          edge.bind(1, n.qname).bind(2, hit.callee).bind(3, name).bind(4, line).bind(5, hit.confidence).run();
          edge.reset();
        }
      }
      refs.reset();
    }
  });
}

}  // namespace hs
