#pragma once
#include "core/model.h"

namespace hs {

class Repo;

// Rebuilds name_index, name_fts and the resolved call graph (edge) for the
// working namespace. Per-definition analysis is cached by hash, so this only
// re-runs name resolution.
void rebuild_index(Repo& repo);

// Gives every definition in ns a lineage id and drops ids of names that no
// longer exist.
void fix_lineage(const Store& s, Namespace& ns);

// The pseudo-definition holding a file's module-level code.
std::string module_node(const std::string& file);

}  // namespace hs
