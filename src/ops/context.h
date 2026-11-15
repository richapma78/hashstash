#pragma once
// Comment regions and context history for definitions, and the combined
// "everything an agent should know about this definition" query.
#include <nlohmann/json.hpp>

#include <string>

#include "ops/repo.h"

namespace hs {

// These change ns in memory; save with repo.set_work_ns.
std::string add_note(Repo& repo, Namespace& ns, const std::string& qname, int from, int to,
                     const std::string& text);
bool remove_note(Repo& repo, Namespace& ns, const std::string& qname, const std::string& id);
std::string add_context(Repo& repo, Namespace& ns, const std::string& qname, const std::string& kind,
                        const std::string& text, const std::string& source);

// Moves a definition's note regions to follow an edit from old_text to new_text.
void remap_notes(Store& s, Namespace& ns, const std::string& lineage, const std::string& old_text,
                 const std::string& new_text);

Attach attach_of(const Store& s, const Namespace& ns, const std::string& lineage);

// Code, signature, notes, context history, tests, callers/callees, change log
// and locks of one definition, as one JSON document.
nlohmann::json context_of(Repo& repo, const std::string& name, bool with_code = true);

}  // namespace hs
