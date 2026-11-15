#include "ops/context.h"

#include <stdexcept>

#include "lang/lang.h"
#include "ops/diff.h"
#include "ops/history.h"
#include "ops/locks.h"
#include "ops/query.h"
#include "ops/tests.h"

namespace hs {

Attach attach_of(const Store& s, const Namespace& ns, const std::string& lineage) {
  auto it = ns.attach.find(lineage);
  if (it == ns.attach.end()) return {};
  return load_attach(s, it->second);
}

static void save_attach(Store& s, Namespace& ns, const std::string& lineage, const Attach& a) {
  if (a.notes.empty() && a.context.empty()) ns.attach.erase(lineage);
  else ns.attach[lineage] = put_attach(s, a);
}

static std::string lineage_for(const Namespace& ns, const std::string& qname) {
  auto it = ns.lineage.find(qname);
  if (it == ns.lineage.end()) throw std::runtime_error("no definition " + qname);
  return it->second;
}

static Hash hash_for(Repo& repo, const std::string& qname) {
  auto r = row_by_qname(repo, qname);
  return r ? r->hash : "";
}

std::string add_note(Repo& repo, Namespace& ns, const std::string& qname, int from, int to,
                     const std::string& text) {
  std::string id = lineage_for(ns, qname);
  auto r = row_by_qname(repo, qname);
  if (!r) throw std::runtime_error("no definition " + qname);
  int lines = count_lines(render_def(repo.store(), r->hash));
  if (from < 1 || to < from || to > lines)
    throw std::runtime_error("lines " + std::to_string(from) + "-" + std::to_string(to) +
                             " are outside " + qname + " (1-" + std::to_string(lines) + ")");
  Attach a = attach_of(repo.store(), ns, id);
  Note n{new_uuid().substr(0, 8), from, to, text, repo.agent(), now_seconds(), false};
  a.notes.push_back(n);
  save_attach(repo.store(), ns, id, a);
  return n.id;
}

bool remove_note(Repo& repo, Namespace& ns, const std::string& qname, const std::string& note) {
  std::string id = lineage_for(ns, qname);
  Attach a = attach_of(repo.store(), ns, id);
  size_t before = a.notes.size();
  std::erase_if(a.notes, [&](const Note& n) { return n.id == note; });
  if (a.notes.size() == before) return false;
  save_attach(repo.store(), ns, id, a);
  return true;
}

std::string add_context(Repo& repo, Namespace& ns, const std::string& qname, const std::string& kind,
                        const std::string& text, const std::string& source) {
  std::string id = lineage_for(ns, qname);
  Attach a = attach_of(repo.store(), ns, id);
  ContextEntry c{new_uuid().substr(0, 8), kind, text, source, repo.agent(), now_seconds(),
                 hash_for(repo, qname)};
  a.context.push_back(c);
  save_attach(repo.store(), ns, id, a);
  return c.id;
}

void remap_notes(Store& s, Namespace& ns, const std::string& lineage, const std::string& old_text,
                 const std::string& new_text) {
  Attach a = attach_of(s, ns, lineage);
  if (a.notes.empty()) return;
  auto map = line_map(old_text, new_text);
  for (auto& n : a.notes) {
    if (n.stale) continue;
    // The region now spans its first to last surviving line, which also takes
    // in any lines inserted between them.
    int f = -1, t = -1;
    for (int l = n.from; l <= n.to && l - 1 < (int)map.size(); ++l)
      if (map[l - 1] >= 0) {
        if (f < 0) f = map[l - 1] + 1;
        t = map[l - 1] + 1;
      }
    if (f < 0) {
      n.stale = true;
      continue;
    }
    n.from = f;
    n.to = t;
  }
  save_attach(s, ns, lineage, a);
}

nlohmann::json context_of(Repo& repo, const std::string& name, bool with_code) {
  IndexRow r = resolve_name(repo, name);
  Store& s = repo.store();
  Namespace ns = repo.work_ns();
  nlohmann::json j = def_summary(repo, r);
  j["lineage"] = r.lineage;
  if (!r.parent.empty()) j["parent"] = r.parent;
  if (with_code) j["code"] = dedent(render_def(s, r.hash));

  Attach a = attach_of(s, ns, r.lineage);
  nlohmann::json notes = nlohmann::json::array(), ctx = nlohmann::json::array();
  for (auto& n : a.notes) {
    nlohmann::json nj = {{"id", n.id}, {"lines", {n.from, n.to}}, {"text", n.text}, {"author", n.author}};
    if (n.stale) nj["stale"] = true;
    notes.push_back(nj);
  }
  for (auto& c : a.context) {
    nlohmann::json cj = {{"kind", c.kind}, {"text", c.text}, {"author", c.author},
                         {"time", format_time(c.time)}, {"version", short_hash(c.def_hash)}};
    if (!c.source.empty()) cj["source"] = c.source;
    ctx.push_back(cj);
  }
  j["notes"] = notes;
  j["context"] = ctx;
  j["tests"] = tests_info(repo, r.qname);
  j["callers"] = callers(repo, r.qname);
  j["callees"] = callees(repo, r.qname);
  try {
    j["history"] = log(repo, 20, r.qname);
  } catch (...) {
    j["history"] = nlohmann::json::array();
  }
  if (auto l = lock_on(repo, r.lineage))
    j["locked_by"] = {{"holder", l->holder}, {"until", format_time(l->expires)}};
  return j;
}

}  // namespace hs
