#include "ops/import_render.h"

#include <set>

#include "ops/build.h"
#include "ops/index.h"

namespace hs {

namespace {

const std::set<std::string> kSkipDirs = {
    ".git", ".hs", ".hg", ".svn", "__pycache__", "node_modules", ".venv", "venv",
    ".mypy_cache", ".pytest_cache", ".ruff_cache", ".tox", ".eggs", ".idea", ".vs", "build", "dist",
    ".godot", ".import"};  // Godot 4 / Godot 3 editor caches (per-asset *.import files are kept)

struct Ignore {
  std::vector<std::string> patterns;  // relative to the import root
  void load(const fs::path& file) {
    if (!fs::exists(file)) return;
    for (auto& raw : split_lines(read_file(file))) {
      std::string l = trim(raw);
      if (l.empty() || l[0] == '#' || l[0] == '!') continue;  // negation not supported yet
      patterns.push_back(l);
    }
  }
  bool match(const std::string& rel, bool dir) const {
    std::string name = rel.substr(rel.rfind('/') + 1);
    for (auto p : patterns) {
      bool dir_only = ends_with(p, "/");
      if (dir_only) {
        if (!dir) continue;
        p.pop_back();
      }
      bool anchored = p.find('/') != std::string::npos;
      if (anchored) {
        if (p[0] == '/') p = p.substr(1);
        if (glob_match(p, rel)) return true;
      } else if (glob_match(p, name)) {
        return true;
      }
    }
    return false;
  }
};

}  // namespace

std::vector<SizeWarning> size_warnings(const Repo& repo, const std::vector<FlatDef>& defs) {
  std::vector<SizeWarning> out;
  int limit = repo.config().warn_lines;
  if (limit <= 0) return out;
  for (auto& d : defs)
    if (d.def.lines > limit && d.def.kind != "namespace")
      out.push_back({d.qname, d.def.lines, limit,
                     d.def.kind == "class"
                         ? "move code out of the class body into methods or helpers"
                         : "split into smaller helper functions"});
  return out;
}

ImportResult import_path(Repo& repo, const fs::path& src_in, const std::string& prefix_in) {
  ImportResult res;
  Store& s = repo.store();
  fs::path src = fs::absolute(src_in);
  if (!fs::exists(src)) throw std::runtime_error("no such path: " + to_generic(src_in));
  std::string prefix = prefix_in;
  while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
  if (!prefix.empty()) prefix += "/";

  Ignore ig;
  fs::path base = fs::is_directory(src) ? src : src.parent_path();
  ig.load(base / ".gitignore");
  ig.load(base / ".hsignore");
  for (auto& p : repo.config().ignore) ig.patterns.push_back(p);

  Namespace ns = repo.work_ns();
  std::vector<std::string> imported;
  auto take = [&](const fs::path& file, const std::string& rel) {
    std::string bytes = read_file(file);
    FileEntry e = store_file(s, prefix + rel, bytes);
    ns.files[prefix + rel] = e;
    imported.push_back(prefix + rel);
    if (e.kind == "file") ++res.files;
    else ++res.blobs;
  };

  s.transaction([&] {
    if (fs::is_regular_file(src)) {
      take(src, to_generic(src.filename()));
    } else {
      fs::path hs_dir = fs::absolute(repo.hs_dir());
      auto it = fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied);
      for (auto end = fs::recursive_directory_iterator(); it != end; ++it) {
        const fs::path& p = it->path();
        std::string rel = to_generic(fs::relative(p, src));
        bool dir = it->is_directory();
        if (dir && (kSkipDirs.count(to_generic(p.filename())) || p == hs_dir || ig.match(rel, true))) {
          it.disable_recursion_pending();
          ++res.skipped;
          continue;
        }
        if (dir || !it->is_regular_file()) continue;
        if (ig.match(rel, false)) {
          ++res.skipped;
          continue;
        }
        take(p, rel);
      }
    }
    fix_lineage(s, ns);
    repo.set_work_ns(ns, "import", "import " + to_generic(src_in));
  });

  std::vector<FlatDef> defs;
  for (auto& f : imported) {
    auto d = flatten_file(s, ns, f);
    res.defs += (int)d.size();
    defs.insert(defs.end(), d.begin(), d.end());
  }
  res.warnings = size_warnings(repo, defs);
  return res;
}

RenderResult render_to(Repo& repo, const fs::path& out_in, const Hash& ns_hash,
                       const std::function<bool(const std::string& path)>& skip) {
  RenderResult res;
  Store& s = repo.store();
  Namespace ns = load_namespace(s, ns_hash.empty() ? repo.work_ns_hash() : ns_hash);
  fs::path out = fs::absolute(out_in);
  std::string key = to_generic(out);
  fs::create_directories(out);

  std::map<std::string, std::string> previous;
  {
    Stmt st(s.db(), "SELECT path, hash FROM render_manifest WHERE out_dir = ?");
    st.bind(1, key);
    while (st.step()) previous[st.text(0)] = st.text(1);
  }
  s.transaction([&] {
    Stmt del(s.db(), "DELETE FROM render_manifest WHERE out_dir = ?");
    del.bind(1, key).run();
    Stmt add(s.db(), "INSERT INTO render_manifest(out_dir, path, hash) VALUES (?, ?, ?)");
    for (auto& [path, e] : ns.files) {
      if (skip && skip(path)) {
        ++res.ignored;
        continue;
      }
      fs::path target = out / from_utf8(path);
      std::string bytes = render_entry(s, e);
      bool same = false;
      if (fs::exists(target)) {
        std::error_code ec;
        if (fs::file_size(target, ec) == bytes.size()) same = read_file(target) == bytes;
      }
      if (same) ++res.unchanged;
      else {
        write_file(target, bytes);
        ++res.written;
      }
      add.bind(1, key).bind(2, path).bind(3, e.hash).run();
      add.reset();
    }
    for (auto& [path, h] : previous) {
      if (ns.files.count(path) && !(skip && skip(path))) continue;
      std::error_code ec;
      if (fs::remove(out / from_utf8(path), ec)) ++res.deleted;
    }
  });
  return res;
}

}  // namespace hs
