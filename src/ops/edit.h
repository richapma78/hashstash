#pragma once
// Changing code in the store (the store is the source of truth).
//
// Edits work on an in-memory Namespace; commit_edits() saves it as the working
// namespace and fills in the review warnings (size, missing tests).
//
// Scratch files (.hs/scratch/edit.<ext>) are the human/CLI route: `hs edit`
// writes locked definitions into one, `hs update` applies it. Sections:
//   # @hs:edit <qname> base=<hash prefix>     replace a definition
//   # @hs:new <file>[:<Container>] [after=<name>]   add definitions
//   # @hs:delete <qname>                      delete a definition
#include <string>
#include <vector>

#include "ops/import_render.h"
#include "ops/repo.h"
#include "ops/tests.h"

namespace hs {

struct EditOutcome {
  bool ok = false;
  std::string error;
  std::vector<std::string> changed;  // qualified names stored by this edit
  std::vector<std::string> removed;
  std::vector<SizeWarning> size_warnings;
  std::vector<TestWarning> test_warnings;
};

// Replaces a definition with new source text. The text may hold several
// definitions (e.g. a function split into helpers): the one with the original
// name (or the only one) keeps the lineage, the rest are new. base is the hash
// (or prefix) the editor started from; "" skips the conflict check.
EditOutcome replace_def(Repo& repo, Namespace& ns, const std::string& qname,
                        const std::string& text, const std::string& base,
                        const std::string& reason = "");
// Adds definitions to a file (created if missing), at top level or inside a
// container, after the named member or after the last one.
EditOutcome add_def(Repo& repo, Namespace& ns, const std::string& file,
                    const std::string& container, const std::string& after,
                    const std::string& text, const std::string& reason = "");
EditOutcome delete_def(Repo& repo, Namespace& ns, const std::string& qname);

// Saves ns as the working namespace and adds size/test warnings to outcomes.
void commit_edits(Repo& repo, const Namespace& ns, std::vector<EditOutcome*> outcomes,
                  const std::string& reason = "");

// Locks the definitions and writes them into scratch files. Returns the files.
std::vector<fs::path> open_for_edit(Repo& repo, const std::vector<std::string>& names);
// Adds an empty "new" section for a file/container to the scratch file.
fs::path open_new(Repo& repo, const std::string& where, const std::string& after);

struct ScratchResult {
  std::string header;  // the section's header line
  EditOutcome outcome;
};
// reason is recorded as the change context of every definition it touches.
std::vector<ScratchResult> apply_scratch(Repo& repo, const std::string& reason = "");
std::vector<fs::path> scratch_files(Repo& repo);

}  // namespace hs
